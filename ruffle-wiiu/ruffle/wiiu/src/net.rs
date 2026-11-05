//! Network for games: connections from the host (host/wiiu/net_wiiu.c, TCP and TLS on threads of their
//! own) and a small HTTP/1.1 client on top of them for URLLoader and friends.
//!
//! The executor (`NullExecutor`, a `LocalPool`) only polls futures that were woken, so futures here that
//! wait on the host park their waker until the next tick: `wake_all` (from `rw_tick`) wakes them.

use crate::host;
use async_channel::{Receiver, Sender, TryRecvError};
use encoding_rs::Encoding;
use indexmap::IndexMap;
use ruffle_core::backend::navigator::{ErrorResponse, NavigationMethod, OwnedFuture, Request, SuccessResponse};
use ruffle_core::loader::Error;
use ruffle_core::socket::{ConnectionState, SocketAction, SocketHandle};
use std::borrow::Cow;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll, Waker};
use std::time::{Duration, Instant};
use url::Url;

thread_local! {
    static WAITING: RefCell<Vec<Waker>> = const { RefCell::new(Vec::new()) };
}

/// Wakes every network future, so each checks its connection once this tick.
pub fn wake_all() {
    let wakers = WAITING.with(|w| std::mem::take(&mut *w.borrow_mut()));
    for w in wakers {
        w.wake();
    }
}

/// Finishes on the next tick.
fn next_tick() -> NextTick {
    NextTick(false)
}

struct NextTick(bool);

impl Future for NextTick {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.0 {
            return Poll::Ready(());
        }
        self.0 = true;
        WAITING.with(|w| w.borrow_mut().push(cx.waker().clone()));
        Poll::Pending
    }
}

const CONNECTING: u32 = 0;
const OPEN: u32 = 1;

/// A host connection, closed when dropped.
struct Sock(u32);

impl Sock {
    fn open(host: &str, port: u16, tls: bool) -> Option<Sock> {
        let id = unsafe { host::host_sock_open(host.as_ptr(), host.len() as u32, port as u32, tls as u32) };
        (id != 0).then_some(Sock(id))
    }

    fn state(&self) -> u32 {
        unsafe { host::host_sock_state(self.0) }
    }

    fn send(&self, data: &[u8]) -> bool {
        data.is_empty() || unsafe { host::host_sock_send(self.0, data.as_ptr(), data.len() as u32) } as usize == data.len()
    }

    fn recv(&self, buf: &mut [u8]) -> usize {
        unsafe { host::host_sock_recv(self.0, buf.as_mut_ptr(), buf.len() as u32) as usize }
    }

    /// Waits until it's open (true) or failed/timed out (false, with the state to report).
    async fn wait_open(&self, timeout: Duration) -> Result<(), ConnectionState> {
        let start = Instant::now();
        loop {
            match self.state() {
                OPEN => return Ok(()),
                CONNECTING if start.elapsed() < timeout => next_tick().await,
                CONNECTING => return Err(ConnectionState::TimedOut),
                _ => return Err(ConnectionState::Failed),
            }
        }
    }
}

impl Drop for Sock {
    fn drop(&mut self) {
        unsafe { host::host_sock_close(self.0) }
    }
}

// ---------------------------------------------------------------- Flash sockets

/// Runs a game's Socket/SecureSocket (port 443 gets TLS: that's how Jackbox's SecureSocket is used).
pub async fn run_socket(
    host: String,
    port: u16,
    timeout: Duration,
    handle: SocketHandle,
    receiver: Receiver<Vec<u8>>,
    sender: Sender<SocketAction>,
) -> Result<(), Error> {
    let Some(sock) = Sock::open(&host, port, port == 443) else {
        let _ = sender.try_send(SocketAction::Connect(handle, ConnectionState::Failed));
        return Ok(());
    };
    if let Err(state) = sock.wait_open(timeout.max(Duration::from_secs(5))).await {
        host::log(1, &format!("net: socket to {host}:{port} {}", if state == ConnectionState::TimedOut { "timed out" } else { "failed" }));
        let _ = sender.try_send(SocketAction::Connect(handle, state));
        return Ok(());
    }
    let _ = sender.try_send(SocketAction::Connect(handle, ConnectionState::Connected));
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        loop {
            match receiver.try_recv() {
                Ok(data) => {
                    if !sock.send(&data) {
                        let _ = sender.try_send(SocketAction::Close(handle));
                        return Ok(());
                    }
                }
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Closed) => return Ok(()), // the game closed it
            }
        }
        // (The state first: data that arrives before the close is still delivered.)
        let state = sock.state();
        loop {
            let n = sock.recv(&mut buf);
            if n == 0 {
                break;
            }
            let _ = sender.try_send(SocketAction::Data(handle, buf[..n].to_vec()));
        }
        if state != OPEN {
            let _ = sender.try_send(SocketAction::Close(handle));
            return Ok(());
        }
        next_tick().await;
    }
}

// ---------------------------------------------------------------- HTTP

struct HttpResponse {
    url: String,
    status: u16,
    redirected: bool,
    body: Option<Vec<u8>>,
}

impl SuccessResponse for HttpResponse {
    fn url(&self) -> Cow<'_, str> {
        Cow::Borrowed(&self.url)
    }

    fn set_url(&mut self, url: String) {
        self.url = url;
    }

    fn body(self: Box<Self>) -> OwnedFuture<Vec<u8>, Error> {
        let body = self.body.unwrap_or_default();
        Box::pin(async move { Ok(body) })
    }

    fn text_encoding(&self) -> Option<&'static Encoding> {
        None
    }

    fn status(&self) -> u16 {
        self.status
    }

    fn redirected(&self) -> bool {
        self.redirected
    }

    fn next_chunk(&mut self) -> OwnedFuture<Option<Vec<u8>>, Error> {
        let chunk = self.body.take();
        Box::pin(async move { Ok(chunk) })
    }

    fn expected_length(&self) -> Result<Option<u64>, Error> {
        Ok(self.body.as_ref().map(|b| b.len() as u64))
    }
}

pub fn is_web_url(url: &str) -> bool {
    url.starts_with("http://") || url.starts_with("https://")
}

pub async fn fetch(request: Request) -> Result<Box<dyn SuccessResponse>, ErrorResponse> {
    let mut url = request.url().to_string();
    let mut method = request.method();
    let mut body = request.body().clone();
    let headers = request.headers().clone();
    let mut redirected = false;
    for _ in 0..6 {
        let fail = |url: &str, why: String| {
            host::log(1, &format!("net: {url}: {why}"));
            ErrorResponse { url: url.to_string(), error: Error::FetchError(why) }
        };
        let parsed = Url::parse(&url).map_err(|e| fail(&url, e.to_string()))?;
        let (status, response_headers, data) =
            request_once(&parsed, method, &body, &headers).await.map_err(|why| fail(&url, why))?;
        host::log(2, &format!("net: {} {url} -> {status}, {} bytes", method_name(method), data.len()));
        if matches!(status, 301 | 302 | 303 | 307 | 308)
            && let Some(location) = header(&response_headers, "location")
            && let Ok(next) = parsed.join(location)
        {
            if status == 303 || (status != 307 && status != 308 && method == NavigationMethod::Post) {
                method = NavigationMethod::Get;
                body = None;
            }
            url = next.to_string();
            redirected = true;
            continue;
        }
        if !(200..300).contains(&status) {
            return Err(ErrorResponse {
                url: url.clone(),
                error: Error::HttpNotOk(url, status, redirected, data.len() as u64),
            });
        }
        return Ok(Box::new(HttpResponse { url, status, redirected, body: Some(data) }));
    }
    Err(ErrorResponse { url, error: Error::FetchError("too many redirects".into()) })
}

fn method_name(method: NavigationMethod) -> &'static str {
    match method {
        NavigationMethod::Get => "GET",
        NavigationMethod::Post => "POST",
    }
}

fn header<'a>(headers: &'a [(String, String)], name: &str) -> Option<&'a str> {
    headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
}

type Response = (u16, Vec<(String, String)>, Vec<u8>);

async fn request_once(
    url: &Url,
    method: NavigationMethod,
    body: &Option<(Vec<u8>, String)>,
    headers: &IndexMap<String, String>,
) -> Result<Response, String> {
    let tls = match url.scheme() {
        "https" => true,
        "http" => false,
        other => return Err(format!("{other}: URLs aren't supported")),
    };
    let host = url.host_str().ok_or("no host in the URL")?;
    let port = url.port_or_known_default().unwrap_or(if tls { 443 } else { 80 });
    let mut target = url.path().to_string();
    if let Some(query) = url.query() {
        target.push('?');
        target.push_str(query);
    }
    let host_header = match url.port() {
        Some(p) => format!("{host}:{p}"),
        None => host.to_string(),
    };
    let mut head = format!(
        "{} {target} HTTP/1.1\r\nHost: {host_header}\r\nUser-Agent: Mozilla/5.0 (Flashplayer Emu)\r\nAccept: */*\r\nConnection: close\r\n",
        method_name(method)
    );
    for (k, v) in headers {
        if ["host", "content-length", "connection", "accept-encoding"].iter().any(|h| k.eq_ignore_ascii_case(h)) {
            continue;
        }
        head.push_str(&format!("{k}: {v}\r\n"));
    }
    let mut payload: &[u8] = &[];
    if method == NavigationMethod::Post {
        if let Some((data, mime)) = body {
            if !headers.keys().any(|k| k.eq_ignore_ascii_case("content-type")) && !mime.is_empty() {
                head.push_str(&format!("Content-Type: {mime}\r\n"));
            }
            payload = data;
        }
        head.push_str(&format!("Content-Length: {}\r\n", payload.len()));
    }
    head.push_str("\r\n");

    let sock = Sock::open(host, port, tls).ok_or("couldn't start a connection")?;
    sock.wait_open(Duration::from_secs(20)).await.map_err(|s| {
        if s == ConnectionState::TimedOut { "connecting timed out".to_string() } else { "couldn't connect".to_string() }
    })?;
    let mut out = head.into_bytes();
    out.extend_from_slice(payload);
    if !sock.send(&out) {
        return Err("sending the request failed".into());
    }

    let mut received = Vec::new();
    let mut buf = vec![0u8; 64 * 1024];
    let mut last_progress = Instant::now();
    loop {
        let state = sock.state();
        let mut got = false;
        loop {
            let n = sock.recv(&mut buf);
            if n == 0 {
                break;
            }
            received.extend_from_slice(&buf[..n]);
            got = true;
        }
        if got {
            last_progress = Instant::now();
            if let Some(done) = parse_response(&received, false) {
                return done;
            }
        }
        if state != OPEN {
            return parse_response(&received, true).unwrap_or_else(|| Err("the connection closed early".into()));
        }
        if last_progress.elapsed() > Duration::from_secs(60) {
            return Err("no answer for 60 s".into());
        }
        next_tick().await;
    }
}

/// The response, once all of it is there (`closed`: nothing more will come).
fn parse_response(data: &[u8], closed: bool) -> Option<Result<Response, String>> {
    let Some(end) = data.windows(4).position(|w| w == b"\r\n\r\n") else {
        return closed.then(|| Err("no response".to_string()));
    };
    let head = String::from_utf8_lossy(&data[..end]);
    let mut lines = head.split("\r\n");
    let status_line = lines.next().unwrap_or("");
    let Some(status) = status_line.split(' ').nth(1).and_then(|s| s.parse::<u16>().ok()) else {
        return Some(Err(format!("bad response: {status_line}")));
    };
    let headers: Vec<(String, String)> = lines
        .filter_map(|l| l.split_once(':'))
        .map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))
        .collect();
    let body = &data[end + 4..];
    if header(&headers, "transfer-encoding").is_some_and(|v| v.to_ascii_lowercase().contains("chunked")) {
        return match dechunk(body) {
            Some(b) => Some(Ok((status, headers, b))),
            None if closed => Some(Err("the connection closed in the middle of the response".into())),
            None => None,
        };
    }
    if let Some(len) = header(&headers, "content-length").and_then(|v| v.parse::<usize>().ok()) {
        if body.len() >= len {
            return Some(Ok((status, headers, body[..len].to_vec())));
        }
        return closed.then(|| Err("the connection closed in the middle of the response".into()));
    }
    if status == 204 || status == 304 {
        return Some(Ok((status, headers, Vec::new())));
    }
    closed.then(|| Ok((status, headers, body.to_vec())))
}

/// A chunked body, once its last chunk has arrived.
fn dechunk(mut data: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    loop {
        let line_end = data.windows(2).position(|w| w == b"\r\n")?;
        let size_text = std::str::from_utf8(&data[..line_end]).ok()?;
        let size = usize::from_str_radix(size_text.split(';').next()?.trim(), 16).ok()?;
        data = &data[line_end + 2..];
        if size == 0 {
            return Some(out);
        }
        if data.len() < size + 2 {
            return None;
        }
        out.extend_from_slice(&data[..size]);
        data = &data[size + 2..];
    }
}
