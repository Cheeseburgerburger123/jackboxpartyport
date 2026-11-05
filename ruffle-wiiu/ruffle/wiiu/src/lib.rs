//! Ruffle for the Wii U: built for wasm32-wasip1, translated to C by wasm2c, linked with a C host.
//! The host drives everything through the `rw_*` exports; see `host.rs` for what it must provide.

mod audio;
mod context3d;
mod filters;
mod host;
mod navigator;
mod net;
mod pixel_bender;
mod render;
mod site;
mod storage;
mod ui;
mod video;

use ruffle_core::backend::audio::AudioMixerProxy;
use ruffle_core::backend::navigator::NullExecutor;
use ruffle_core::config::Letterbox;
use ruffle_core::events::{
    KeyDescriptor, KeyLocation, LogicalKey, MouseButton, MouseWheelDelta, NamedKey, PhysicalKey,
};
use ruffle_core::{
    FloatDuration, Player, PlayerBuilder, PlayerEvent, StageAlign, StageScaleMode,
};
use std::sync::{Arc, Mutex};

struct State {
    player: Arc<Mutex<Player>>,
    executor: NullExecutor,
    audio: AudioMixerProxy,
}

static mut STATE: Option<State> = None;

#[allow(static_mut_refs)]
fn state() -> Option<&'static mut State> {
    // SAFETY: the host calls the exports from one thread only.
    unsafe { STATE.as_mut() }
}

struct HostLog;

impl tracing::Subscriber for HostLog {
    fn enabled(&self, meta: &tracing::Metadata<'_>) -> bool {
        *meta.level() <= tracing::Level::INFO
    }
    fn new_span(&self, _: &tracing::span::Attributes<'_>) -> tracing::span::Id {
        tracing::span::Id::from_u64(1)
    }
    fn record(&self, _: &tracing::span::Id, _: &tracing::span::Record<'_>) {}
    fn record_follows_from(&self, _: &tracing::span::Id, _: &tracing::span::Id) {}
    fn event(&self, event: &tracing::Event<'_>) {
        struct Msg(String);
        impl tracing::field::Visit for Msg {
            fn record_debug(&mut self, field: &tracing::field::Field, value: &dyn std::fmt::Debug) {
                use std::fmt::Write;
                if field.name() == "message" {
                    let _ = write!(self.0, "{value:?}");
                } else {
                    let _ = write!(self.0, " {}={value:?}", field.name());
                }
            }
        }
        let mut msg = Msg(format!("[{}] ", event.metadata().target()));
        event.record(&mut msg);
        let level = match *event.metadata().level() {
            tracing::Level::ERROR => 0,
            tracing::Level::WARN => 1,
            _ => 2,
        };
        host::log(level, &msg.0);
    }
    fn enter(&self, _: &tracing::span::Id) {}
    fn exit(&self, _: &tracing::span::Id) {}
}

/// Buffer the host can copy strings or files into before passing them to another export.
#[unsafe(no_mangle)]
pub extern "C" fn rw_alloc(len: u32) -> *mut u8 {
    let mut v = vec![0u8; len as usize].into_boxed_slice();
    let p = v.as_mut_ptr();
    std::mem::forget(v);
    p
}

#[unsafe(no_mangle)]
pub extern "C" fn rw_free(ptr: *mut u8, len: u32) {
    unsafe { drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len as usize))) }
}

/// Starts playing `url` (e.g. "file:///swf/game.swf"; files are read through WASI) on a
/// `width`x`height` viewport. Returns 0 on success.
#[unsafe(no_mangle)]
pub extern "C" fn rw_start(url: *const u8, url_len: u32, width: u32, height: u32) -> i32 {
    let _ = tracing::subscriber::set_global_default(HostLog);
    std::panic::set_hook(Box::new(|info| host::log(0, &format!("panic: {info}"))));

    let url = unsafe { std::slice::from_raw_parts(url, url_len as usize) };
    let Ok(url) = std::str::from_utf8(url) else {
        return 1;
    };
    // "<movie url>\n<spoofed url>\n<web page>": the address sitelocked games are told they run from, and the page
    // that embeds a game saved with its website. Either can be empty.
    let mut lines = url.split('\n');
    let url = lines.next().unwrap_or("");
    let spoof = lines.next().filter(|s| !s.is_empty()).map(str::to_string);
    let settings = lines.next().filter(|s| !s.is_empty()).map_or_else(Vec::new, |page| {
        let settings = site::page_settings(page, url);
        host::log(2, &format!("{page}: {} settings for the game", settings.len()));
        settings
    });
    let executor = NullExecutor::new();
    let navigator = navigator::HostNavigator::new(executor.spawner(), url);
    let (audio, proxy) = audio::HostAudioBackend::new();
    let player = PlayerBuilder::new()
        .with_renderer(render::HostRenderer::new(width, height))
        .with_audio(audio)
        .with_navigator(navigator)
        .with_storage(Box::new(storage::DiskStorage::new()))
        .with_ui(ui::HostUi::new())
        .with_video(video::HostVideoBackend::new())
        .with_viewport_dimensions(width, height, 1.0)
        .with_letterbox(Letterbox::On)
        // A TV/GamePad screen: always fit the whole stage, centered, whatever the movie asks for.
        .with_scale_mode(StageScaleMode::ShowAll, true)
        .with_align(StageAlign::empty(), true)
        .with_autoplay(true)
        .with_default_font(true)
        .with_spoofed_url(spoof)
        .build();
    player.lock().unwrap().set_gc_sleep_factor(1.5);
    host::log(2, &format!("rw_start {url} {width}x{height}"));
    host::log(2, "jackbox build: patched Ruffle core");
    player.lock().unwrap().fetch_root_movie(
        url.to_string(),
        settings,
        Box::new(|header| {
            host::log(
                2,
                &format!(
                    "movie loaded: SWF {} {}x{} @ {} fps",
                    header.version(),
                    header.stage_size().width().to_pixels(),
                    header.stage_size().height().to_pixels(),
                    header.frame_rate()
                ),
            )
        }),
    );
    unsafe {
        STATE = Some(State {
            player,
            executor,
            audio: proxy,
        })
    };
    0
}

/// Advances the movie by `dt_ms` of wall time. Returns 1 if a new frame should be rendered.
#[unsafe(no_mangle)]
pub extern "C" fn rw_tick(dt_ms: f64) -> i32 {
    let Some(s) = state() else { return 0 };
    net::wake_all();
    s.executor.run();
    let mut player = s.player.lock().unwrap();
    player.tick(FloatDuration::from_millis(dt_ms));
    drop(player);
    s.executor.run();
    s.player.lock().unwrap().needs_render() as i32
}

/// Frees all the memory the game no longer uses, for when it's nearly out.
#[unsafe(no_mangle)]
pub extern "C" fn rw_collect() {
    if let Some(s) = state() {
        s.player.lock().unwrap().collect_garbage();
    }
}

/// How much of the root movie has been loaded, 0..=1000 (1000 = fully loaded).
#[unsafe(no_mangle)]
pub extern "C" fn rw_load_permille() -> u32 {
    let Some(s) = state() else { return 0 };
    s.player.lock().unwrap().mutate_with_update_context(|context| {
        let Some(root) = context.stage.root_clip().and_then(|r| r.as_movie_clip()) else {
            return 0;
        };
        let total = root.total_bytes().max(0) as u64;
        if total == 0 {
            return 0;
        }
        ((root.loaded_bytes() as u64 * 1000) / total).min(1000) as u32
    })
}

/// The movie's stage size in pixels as width << 16 | height, 0 before it has loaded.
#[unsafe(no_mangle)]
pub extern "C" fn rw_stage_size() -> u32 {
    let Some(s) = state() else { return 0 };
    let mut player = s.player.lock().unwrap();
    let (w, h) = (player.movie_width(), player.movie_height());
    (w.min(0xffff) << 16) | h.min(0xffff)
}

#[unsafe(no_mangle)]
pub extern "C" fn rw_render() {
    if let Some(s) = state() {
        s.player.lock().unwrap().render();
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rw_resize(width: u32, height: u32) {
    if let Some(s) = state() {
        let dims = ruffle_render::backend::ViewportDimensions {
            width,
            height,
            scale_factor: 1.0,
        };
        s.player.lock().unwrap().set_viewport_dimensions(dims);
    }
}

/// kind: 0 move, 1 down, 2 up, 3 wheel (x = lines). button: 0 left, 1 middle, 2 right.
#[unsafe(no_mangle)]
pub extern "C" fn rw_mouse(kind: u32, x: f64, y: f64, button: u32) -> i32 {
    let Some(s) = state() else { return 0 };
    let button = match button {
        1 => MouseButton::Middle,
        2 => MouseButton::Right,
        _ => MouseButton::Left,
    };
    let event = match kind {
        0 => PlayerEvent::MouseMove { x, y },
        1 => PlayerEvent::MouseDown {
            x,
            y,
            button,
            index: None,
        },
        2 => PlayerEvent::MouseUp { x, y, button },
        _ => PlayerEvent::MouseWheel {
            delta: MouseWheelDelta::Lines(x),
        },
    };
    s.player.lock().unwrap().handle_event(event) as i32
}

/// Named keys the host can send with `rw_key` (codes below 0x100 are ASCII characters).
const KEY_NAMED: [(u32, NamedKey, PhysicalKey); 13] = [
    (0x100, NamedKey::ArrowLeft, PhysicalKey::ArrowLeft),
    (0x101, NamedKey::ArrowRight, PhysicalKey::ArrowRight),
    (0x102, NamedKey::ArrowUp, PhysicalKey::ArrowUp),
    (0x103, NamedKey::ArrowDown, PhysicalKey::ArrowDown),
    (0x104, NamedKey::Enter, PhysicalKey::Enter),
    (0x105, NamedKey::Escape, PhysicalKey::Escape),
    (0x106, NamedKey::Backspace, PhysicalKey::Backspace),
    (0x107, NamedKey::Tab, PhysicalKey::Tab),
    (0x108, NamedKey::Shift, PhysicalKey::ShiftLeft),
    (0x109, NamedKey::Control, PhysicalKey::ControlLeft),
    (0x10a, NamedKey::Delete, PhysicalKey::Delete),
    (0x10b, NamedKey::PageUp, PhysicalKey::PageUp),
    (0x10c, NamedKey::PageDown, PhysicalKey::PageDown),
];

fn char_physical(c: char) -> PhysicalKey {
    use PhysicalKey::*;
    match c.to_ascii_lowercase() {
        'a' => KeyA, 'b' => KeyB, 'c' => KeyC, 'd' => KeyD, 'e' => KeyE, 'f' => KeyF,
        'g' => KeyG, 'h' => KeyH, 'i' => KeyI, 'j' => KeyJ, 'k' => KeyK, 'l' => KeyL,
        'm' => KeyM, 'n' => KeyN, 'o' => KeyO, 'p' => KeyP, 'q' => KeyQ, 'r' => KeyR,
        's' => KeyS, 't' => KeyT, 'u' => KeyU, 'v' => KeyV, 'w' => KeyW, 'x' => KeyX,
        'y' => KeyY, 'z' => KeyZ,
        '0' => Digit0, '1' => Digit1, '2' => Digit2, '3' => Digit3, '4' => Digit4,
        '5' => Digit5, '6' => Digit6, '7' => Digit7, '8' => Digit8, '9' => Digit9,
        ' ' => Space, '-' => Minus, '=' => Equal, ',' => Comma, '.' => Period, '/' => Slash,
        ';' => Semicolon, '\'' => Quote, '[' => BracketLeft, ']' => BracketRight,
        '\\' => Backslash, '`' => Backquote,
        _ => Unknown,
    }
}

/// `code`: ASCII character, or one of the 0x100+ named keys above.
#[unsafe(no_mangle)]
pub extern "C" fn rw_key(down: u32, code: u32) {
    let Some(s) = state() else { return };
    let key = if let Some((_, named, physical)) = KEY_NAMED.iter().find(|k| k.0 == code) {
        KeyDescriptor {
            physical_key: *physical,
            logical_key: LogicalKey::Named(*named),
            key_location: KeyLocation::Standard,
        }
    } else {
        let c = char::from_u32(code).unwrap_or('\0');
        KeyDescriptor {
            physical_key: char_physical(c),
            logical_key: LogicalKey::Character(c),
            key_location: KeyLocation::Standard,
        }
    };
    let mut player = s.player.lock().unwrap();
    if down != 0 {
        player.handle_event(PlayerEvent::KeyDown { key });
        if code < 0x100 && code >= 0x20 {
            if let Some(c) = char::from_u32(code) {
                player.handle_event(PlayerEvent::TextInput { codepoint: c });
            }
        }
    } else {
        player.handle_event(PlayerEvent::KeyUp { key });
    }
}

/// Non-zero while an editable text field has focus and wants the on-screen keyboard; the value changes
/// each time Ruffle asks for it again (the field was focused, or clicked while focused).
#[unsafe(no_mangle)]
pub extern "C" fn rw_keyboard() -> u32 {
    ui::keyboard_request()
}

/// The focused text field's text as UTF-8 into `out` (at most `cap` bytes); returns the full length,
/// or u32::MAX when no text field has focus. `info` gets max chars (0 = any) | 0x10000 password |
/// 0x20000 multiline.
#[unsafe(no_mangle)]
pub extern "C" fn rw_text_get(out: *mut u8, cap: u32, info: *mut u32) -> u32 {
    let Some(s) = state() else { return u32::MAX };
    let found = s.player.lock().unwrap().mutate_with_update_context(|context| {
        context.focus_tracker.get_as_edit_text().map(|t| {
            let flags = (t.max_chars().clamp(0, 0xffff) as u32)
                | if t.is_password() { 0x10000 } else { 0 }
                | if t.is_multiline() { 0x20000 } else { 0 };
            (t.text().to_string(), flags)
        })
    });
    let Some((text, flags)) = found else { return u32::MAX };
    unsafe {
        *info = flags;
        let n = text.len().min(cap as usize);
        std::ptr::copy_nonoverlapping(text.as_ptr(), out, n);
    }
    text.len() as u32
}

/// Replaces the focused text field's contents with the UTF-8 `text`, as if typed: select all, delete,
/// then each character (so the field's restrict / maxChars and change events apply).
#[unsafe(no_mangle)]
pub extern "C" fn rw_text_set(text: *const u8, len: u32) {
    use ruffle_core::events::TextControlCode;
    let Some(s) = state() else { return };
    let text = unsafe { std::slice::from_raw_parts(text, len as usize) };
    let text = String::from_utf8_lossy(text);
    let mut player = s.player.lock().unwrap();
    player.handle_event(PlayerEvent::TextControl { code: TextControlCode::SelectAll });
    player.handle_event(PlayerEvent::TextControl { code: TextControlCode::Backspace });
    for c in text.chars() {
        if c == '\n' {
            player.handle_event(PlayerEvent::TextControl { code: TextControlCode::Enter });
        } else if !c.is_control() {
            player.handle_event(PlayerEvent::TextInput { codepoint: c });
        }
    }
}

/// Drawing statistics since the last call, into out[0..8] (see filters::ST_*).
#[unsafe(no_mangle)]
pub extern "C" fn rw_render_stats(out: *mut u32) {
    for (i, s) in filters::STATS.iter().enumerate() {
        unsafe { *out.add(i) = s.swap(0, std::sync::atomic::Ordering::Relaxed) };
    }
}

/// Text from an on-screen keyboard (one codepoint at a time).
#[unsafe(no_mangle)]
pub extern "C" fn rw_text(codepoint: u32) {
    let Some(s) = state() else { return };
    if let Some(c) = char::from_u32(codepoint) {
        s.player
            .lock()
            .unwrap()
            .handle_event(PlayerEvent::TextInput { codepoint: c });
    }
}

/// Fills `frames` stereo frames of signed 16-bit audio at `rw_audio_rate()` Hz.
#[unsafe(no_mangle)]
pub extern "C" fn rw_audio_mix(out: *mut i16, frames: u32) {
    let buf = unsafe { std::slice::from_raw_parts_mut(out, frames as usize * 2) };
    buf.fill(0);
    if let Some(s) = state() {
        s.audio.mix::<i16>(buf);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rw_audio_rate() -> u32 {
    audio::SAMPLE_RATE
}
