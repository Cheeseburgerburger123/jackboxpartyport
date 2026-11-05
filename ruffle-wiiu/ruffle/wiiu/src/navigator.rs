//! Navigator: local files, plus web URLs and sockets through net.rs. Paths are WASI paths: "/" is the host's root folder (sd: on Wii U).

use crate::host;
use async_channel::{Receiver, Sender};
use indexmap::IndexMap;
use ruffle_core::backend::navigator::{
    ErrorResponse, NavigationMethod, NavigatorBackend, NullSpawner, OwnedFuture, Request,
    SuccessResponse, fetch_path, resolve_url_with_relative_base_path,
};
use ruffle_core::loader::Error;
use ruffle_core::socket::{SocketAction, SocketHandle};
use std::path::{Path, PathBuf};
use std::time::Duration;
use url::{ParseError, Url};

pub struct HostNavigator {
    spawner: NullSpawner,
    /// The movie's folder: relative URLs ("levels.xml") load from next to the SWF, like desktop Ruffle.
    base_path: PathBuf,
    /// A game's own folder (swf/<folder>), which stands in for the top of its website.
    site_root: Option<PathBuf>,
}

impl HostNavigator {
    pub fn new(spawner: NullSpawner, movie_url: &str) -> Self {
        let base_path = movie_folder(movie_url);
        let site_root = site_root(&base_path);
        Self { spawner, base_path, site_root }
    }

    /// Where a game saved with its website finds a file the browser would have found: "/flash/a.pak" and
    /// "http://its.site/flash/a.pak" start at the site's top, and relative links at the page, not the SWF.
    fn on_site(&self, url: &str) -> Option<String> {
        let parsed = self.resolve_url(url).ok()?;
        // Games wait for these online files before they start (a portal's API, an ad): the app carries
        // stand-ins, whose services do nothing. (content/stubs; Mochi is blocked by the engine itself, and
        // games handle that.)
        const STAND_INS: &[(&str, &str, &str)] = &[
            ("agi.armorgames.com", "/AGI.swf", "AGI.swf"),
            ("kongregate.com", "/API_AS3_Local.swf", "Kongregate.swf"),
            ("cpmstar.com", "/adviewas3.swf", "Blank.swf"),
            ("cpmstar.com", "/adviewas2.swf", "BlankAS2.swf"),
            ("gunblood.com", ".swf", "BlankAS2.swf"),
        ];
        if let Some(host) = parsed.host_str() {
            for (site, ending, file) in STAND_INS {
                if (host == *site || host.ends_with(&format!(".{site}"))) && parsed.path().ends_with(ending) {
                    host::log(2, &format!("online file {parsed} replaced by the app's stand-in {file}"));
                    return Some(format!("file:///.content/stubs/{file}"));
                }
            }
        }        let root = self.site_root.as_ref()?;
        let path = percent_decode(parsed.path());
        let relative = match parsed.scheme() {
            "file" => {
                if std::fs::metadata(&path).is_ok_and(|m| m.is_file()) {
                    return None;
                }
                path.strip_prefix(self.base_path.to_str()?).unwrap_or(&path)
            }
            "http" | "https" => &path,
            _ => return None,
        };
        let local = root.join(relative.trim_start_matches('/'));
        if !std::fs::metadata(&local).is_ok_and(|m| m.is_file()) {
            return None;
        }
        let mut found = Url::parse("file:///").ok()?;
        found.set_path(local.to_str()?);
        Some(found.into())
    }
}

/// "/wiiu/apps/x/swf/game/flash" -> "/wiiu/apps/x/swf/game"; none for an SWF directly in swf/.
fn site_root(movie_folder: &Path) -> Option<PathBuf> {
    let folder = movie_folder.to_str()?;
    let after = folder.find("/swf/")? + "/swf/".len();
    let end = folder[after..].find('/').map_or(folder.len(), |at| after + at);
    Some(PathBuf::from(&folder[..end]))
}

/// "file:///wiiu/apps/x/swf/en/game.swf" -> "/wiiu/apps/x/swf/en" (percent-decoded); "/" if it isn't a file URL.
fn movie_folder(movie_url: &str) -> PathBuf {
    let Some(path) = movie_url.strip_prefix("file://") else {
        return PathBuf::from("/");
    };
    let path = path.split(['?', '#']).next().unwrap_or("");
    let Some(slash) = path.rfind('/') else {
        return PathBuf::from("/");
    };
    PathBuf::from(percent_decode(&path[..slash.max(1)]))
}

pub fn percent_decode(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        let hex = |b: u8| (b as char).to_digit(16);
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let (Some(h), Some(l)) = (hex(bytes[i + 1]), hex(bytes[i + 2])) {
                out.push((h * 16 + l) as u8);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

impl NavigatorBackend for HostNavigator {
    fn navigate_to_url(
        &self,
        url: &str,
        _target: &str,
        _vars_method: Option<(NavigationMethod, IndexMap<String, String>)>,
    ) {
        host::log(2, &format!("navigate_to_url ignored: {url}"));
    }

    fn fetch(&self, request: Request) -> OwnedFuture<Box<dyn SuccessResponse>, ErrorResponse> {
        let on_site = self.on_site(request.url());
        if on_site.is_none() && crate::net::is_web_url(request.url()) {
            return Box::pin(crate::net::fetch(request));
        }
        fetch_path(self, "HostNavigator", on_site.as_deref().unwrap_or(request.url()), None)
    }

    fn resolve_url(&self, url: &str) -> Result<Url, ParseError> {
        resolve_url_with_relative_base_path(self, self.base_path.clone(), url)
    }

    fn spawn_future(&mut self, future: OwnedFuture<(), Error>) {
        self.spawner.spawn_local(future);
    }

    fn pre_process_url(&self, url: Url) -> Url {
        url
    }

    fn connect_socket(
        &mut self,
        host: String,
        port: u16,
        timeout: Duration,
        handle: SocketHandle,
        receiver: Receiver<Vec<u8>>,
        sender: Sender<SocketAction>,
    ) {
        host::log(2, &format!("net: game opens a socket to {host}:{port}"));
        self.spawner.spawn_local(Box::pin(crate::net::run_socket(host, port, timeout, handle, receiver, sender)));
    }
}
