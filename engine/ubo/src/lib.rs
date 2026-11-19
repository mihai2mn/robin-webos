// SPDX-License-Identifier: GPL-3.0-or-later
//! C interface to adblock-rust for Wren's web process extension (engine/ubo/extension.c).
//!
//! Engines are built ahead of time by `wren-ubo-compile` so the TV only deserializes them: the core (engine.dat:
//! uBO's default lists) and one per extra list (lists/<name>.dat, each with uBO's Unbreak and Quick fixes inside),
//! which wren://adblock turns on and off. As in Brave, which also keeps one engine per list group, a request is
//! checked against the core first, then each extra list; exceptions apply within their own engine.
//! Strings cross the boundary as NUL-terminated UTF-8; strings returned to C go back through
//! `wren_ubo_string_free`.
use adblock::request::Request;
use adblock::resources::Resource;
use adblock::Engine;
use std::collections::{HashMap, HashSet};
use std::ffi::{c_char, CStr, CString};
use std::ptr;

pub struct WrenUbo {
    resources: Vec<Resource>,
    /// (name, engine); the core is first and is named "core".
    engines: Vec<(String, Engine)>,
}

/// What to do with a request.
pub const WREN_UBO_ALLOW: i32 = 0;
/// Cancel it.
pub const WREN_UBO_BLOCK: i32 = 1;
/// Load `text` (a data: URL of a uBO neutered resource) instead.
pub const WREN_UBO_REDIRECT: i32 = 2;
/// Load `text` (the same URL without tracking parameters, `$removeparam`) instead.
pub const WREN_UBO_REWRITE: i32 = 3;

#[repr(C)]
pub struct WrenUboVerdict {
    pub action: i32,
    pub text: *mut c_char,
}

/// Cosmetic resources for one document, from every engine.
#[repr(C)]
pub struct WrenUboCosmetic {
    /// A style sheet hiding this site's specific selectors ("" when none).
    pub css: *mut c_char,
    /// Scriptlets for the page's main world ("" when none); run before the page's own scripts.
    pub script: *mut c_char,
    /// JSON array of procedural filters (`:has-text`, `:upward`, `:remove`, ...), "[]" when none.
    pub procedural: *mut c_char,
    /// JSON object: engine name -> its class/id exceptions on this site, or null where generic hiding is off
    /// (`$generichide`). Give back to `wren_ubo_hidden`.
    pub exceptions: *mut c_char,
    /// Nonzero when no engine does generic hiding on the site.
    pub generichide: i32,
}

fn str_of<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        return "";
    }
    unsafe { CStr::from_ptr(p) }.to_str().unwrap_or("")
}

fn c_string(s: String) -> *mut c_char {
    // Selectors and scripts never contain NUL; if one did, drop it rather than fail the whole result.
    CString::new(s.replace('\0', "")).map(CString::into_raw).unwrap_or(ptr::null_mut())
}

const HIDE_DECL: &str = "{display:none!important}";

/// One rule per selector: an invalid selector invalidates its whole rule, so one WebKit cannot parse must not
/// take others down with it. (A page gets tens to a few hundred selectors, so the repetition costs little.)
fn style_sheet<'a>(selectors: impl IntoIterator<Item = &'a String>) -> String {
    let mut css = String::new();
    for s in selectors {
        css.push_str(s);
        css.push_str(HIDE_DECL);
        css.push('\n');
    }
    css
}

fn load_engine(dat: &[u8], resources: &[Resource]) -> Option<Engine> {
    let mut engine = Engine::default();
    engine.deserialize(dat).ok()?;
    engine.use_resources(resources.iter().cloned());
    Some(engine)
}

/// Loads the core engine (engine.dat) and the uBO resources (resources.json: redirect targets and scriptlets).
/// Returns null when either is unusable.
#[no_mangle]
pub extern "C" fn wren_ubo_new(dat: *const u8, dat_len: usize, resources_json: *const c_char) -> *mut WrenUbo {
    if dat.is_null() || dat_len == 0 {
        return ptr::null_mut();
    }
    let resources: Vec<Resource> = match serde_json::from_str(str_of(resources_json)) {
        Ok(r) => r,
        Err(_) => return ptr::null_mut(),
    };
    let bytes = unsafe { std::slice::from_raw_parts(dat, dat_len) };
    match load_engine(bytes, &resources) {
        Some(core) => Box::into_raw(Box::new(WrenUbo { resources, engines: vec![("core".into(), core)] })),
        None => ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn wren_ubo_free(ubo: *mut WrenUbo) {
    if !ubo.is_null() {
        drop(unsafe { Box::from_raw(ubo) });
    }
}

/// Adds (or replaces) an extra list's engine. Returns 1 when loaded, 0 when the data is unusable.
#[no_mangle]
pub extern "C" fn wren_ubo_add_list(ubo: *mut WrenUbo, name: *const c_char, dat: *const u8, dat_len: usize) -> i32 {
    let Some(ubo) = (unsafe { ubo.as_mut() }) else { return 0 };
    let name = str_of(name);
    if dat.is_null() || dat_len == 0 || name.is_empty() || name == "core" {
        return 0;
    }
    let bytes = unsafe { std::slice::from_raw_parts(dat, dat_len) };
    let Some(engine) = load_engine(bytes, &ubo.resources) else { return 0 };
    ubo.engines.retain(|(n, _)| n != name);
    ubo.engines.push((name.to_owned(), engine));
    1
}

/// Removes an extra list's engine (its memory is freed). Returns 1 when it was loaded.
#[no_mangle]
pub extern "C" fn wren_ubo_remove_list(ubo: *mut WrenUbo, name: *const c_char) -> i32 {
    let Some(ubo) = (unsafe { ubo.as_mut() }) else { return 0 };
    let name = str_of(name);
    let before = ubo.engines.len();
    ubo.engines.retain(|(n, _)| n == "core" || n != name);
    (ubo.engines.len() != before) as i32
}

/// Whether an extra list's engine is loaded.
#[no_mangle]
pub extern "C" fn wren_ubo_has_list(ubo: *const WrenUbo, name: *const c_char) -> i32 {
    let Some(ubo) = (unsafe { ubo.as_ref() }) else { return 0 };
    let name = str_of(name);
    ubo.engines.iter().any(|(n, _)| n == name) as i32
}

#[no_mangle]
pub extern "C" fn wren_ubo_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

/// Checks one request. `request_type` is uBO/adblock-rust's name for it ("script", "image", "stylesheet", "xhr",
/// "sub_frame", "font", "media", "ping", "other", ...); `source_url` is the page (top-level document) making it.
#[no_mangle]
pub extern "C" fn wren_ubo_check(
    ubo: *const WrenUbo,
    url: *const c_char,
    source_url: *const c_char,
    request_type: *const c_char,
) -> WrenUboVerdict {
    let allow = WrenUboVerdict { action: WREN_UBO_ALLOW, text: ptr::null_mut() };
    let Some(ubo) = (unsafe { ubo.as_ref() }) else { return allow };
    let Ok(request) = Request::new(str_of(url), str_of(source_url), str_of(request_type), "GET") else {
        return allow;
    };
    let mut rewritten = None;
    for (_, engine) in &ubo.engines {
        let result = engine.check_network_request(&request);
        if result.should_block() {
            // A redirect replaces the blocked resource with uBO's neutered version (an empty script, a stub of
            // the ad library, a 1x1 GIF, ...), so pages waiting on it keep working.
            if let Some(redirect) = result.redirect {
                return WrenUboVerdict { action: WREN_UBO_REDIRECT, text: c_string(redirect) };
            }
            return WrenUboVerdict { action: WREN_UBO_BLOCK, text: ptr::null_mut() };
        }
        if rewritten.is_none() {
            rewritten = result.rewritten_url;
        }
    }
    match rewritten {
        Some(r) => WrenUboVerdict { action: WREN_UBO_REWRITE, text: c_string(r) },
        None => allow,
    }
}

/// The cosmetic resources (site-specific hiding, scriptlets, procedural filters) for a document at `url`.
#[no_mangle]
pub extern "C" fn wren_ubo_cosmetic(ubo: *const WrenUbo, url: *const c_char) -> WrenUboCosmetic {
    let empty = || WrenUboCosmetic {
        css: c_string(String::new()),
        script: c_string(String::new()),
        procedural: c_string("[]".into()),
        exceptions: c_string("{}".into()),
        generichide: 1,
    };
    let Some(ubo) = (unsafe { ubo.as_ref() }) else { return empty() };
    let mut hide: HashSet<String> = HashSet::new();
    let mut procedural: HashSet<String> = HashSet::new();
    let mut script = String::new();
    let mut exceptions: HashMap<&str, Option<Vec<String>>> = HashMap::new();
    let mut all_generichide = true;
    for (name, engine) in &ubo.engines {
        let r = engine.url_cosmetic_resources(str_of(url));
        hide.extend(r.hide_selectors);
        procedural.extend(r.procedural_actions);
        if !r.injected_script.is_empty() {
            script.push_str(&r.injected_script);
            script.push('\n');
        }
        all_generichide &= r.generichide;
        exceptions.insert(name, if r.generichide { None } else { Some(r.exceptions.into_iter().collect()) });
    }
    let procedural: Vec<&String> = procedural.iter().collect();
    WrenUboCosmetic {
        css: c_string(style_sheet(hide.iter())),
        script: c_string(script),
        procedural: c_string(serde_json::to_string(&procedural).unwrap_or_else(|_| "[]".into())),
        exceptions: c_string(serde_json::to_string(&exceptions).unwrap_or_else(|_| "{}".into())),
        generichide: all_generichide as i32,
    }
}

#[no_mangle]
pub extern "C" fn wren_ubo_cosmetic_free(c: *mut WrenUboCosmetic) {
    if let Some(c) = unsafe { c.as_mut() } {
        for s in [&mut c.css, &mut c.script, &mut c.procedural, &mut c.exceptions] {
            wren_ubo_string_free(*s);
            *s = ptr::null_mut();
        }
    }
}

/// Generic hiding: the style sheet for the classes and ids seen in a document (newline-separated, without "." or
/// "#"). `exceptions_json` is `WrenUboCosmetic::exceptions` of the same document. "" when nothing applies.
#[no_mangle]
pub extern "C" fn wren_ubo_hidden(
    ubo: *const WrenUbo,
    classes: *const c_char,
    ids: *const c_char,
    exceptions_json: *const c_char,
) -> *mut c_char {
    let Some(ubo) = (unsafe { ubo.as_ref() }) else { return c_string(String::new()) };
    let exceptions: HashMap<String, Option<HashSet<String>>> =
        serde_json::from_str(str_of(exceptions_json)).unwrap_or_default();
    let split = |p| str_of(p).split('\n').filter(|s| !s.is_empty()).collect::<Vec<_>>();
    let (classes, ids) = (split(classes), split(ids));
    let mut selectors: HashSet<String> = HashSet::new();
    for (name, engine) in &ubo.engines {
        // An engine added after the document's cosmetic data was made has no entry: no exceptions for it.
        let exc = match exceptions.get(name) {
            Some(None) => continue, // generic hiding off on this site for that engine
            Some(Some(e)) => e.clone(),
            None => HashSet::new(),
        };
        selectors.extend(engine.hidden_class_id_selectors(&classes, &ids, &exc));
    }
    c_string(style_sheet(selectors.iter()))
}
