// SPDX-License-Identifier: GPL-3.0-or-later
//! wren-ubo-compile: builds what the TV loads (build time, on the build host).
//!
//!   wren-ubo-compile OUT_DIR WAR_DIR REDIRECT_RESOURCES_JS SCRIPTLETS_JSON LIST...
//!
//! LIST is `path`, optionally followed by `:trusted` (uBO's own lists: may use trusted scriptlets such as
//! `trusted-set-constant`, as in uBO) and/or `:hosts` (a hosts file, e.g. Peter Lowe's list).
//! Writes OUT_DIR/engine.dat (the parsed filters, adblock-rust's own format) and OUT_DIR/resources.json (uBO's
//! redirect resources from WAR_DIR + REDIRECT_RESOURCES_JS, and its scriptlets from SCRIPTLETS_JSON, made by
//! tools/ubo/scriptlets-to-json.mjs). Then reloads both the way the TV does and runs a few checks.
use adblock::lists::{FilterFormat, FilterSet, ParseOptions};
use adblock::request::Request;
use adblock::resources::resource_assembler::assemble_web_accessible_resources;
use adblock::resources::{PermissionMask, Resource};
use adblock::Engine;
use std::path::Path;
use std::process::ExitCode;

/// uBO's "trusted source" permission: bit 0, matching `requiresTrust` in tools/ubo/scriptlets-to-json.mjs.
const TRUSTED: PermissionMask = PermissionMask::from_bits(1);

/// Parses LIST[:trusted][:hosts] arguments into a filter set.
fn filter_set(specs: &[String]) -> Result<FilterSet, String> {
    let mut set = FilterSet::new(false);
    for spec in specs {
        let mut parts = spec.split(':');
        let path = parts.next().unwrap();
        let flags: Vec<&str> = parts.collect();
        let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
        let opts = ParseOptions {
            format: if flags.contains(&"hosts") { FilterFormat::Hosts } else { FilterFormat::Standard },
            permissions: if flags.contains(&"trusted") { TRUSTED } else { PermissionMask::default() },
            ..Default::default()
        };
        set.add_filter_list(text, opts);
    }
    Ok(set)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    // wren-ubo-compile --list-engine OUT.dat LIST...: one extra list's own engine (wren://adblock turns it on).
    if args.len() >= 4 && args[1] == "--list-engine" {
        let set = match filter_set(&args[3..]) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("{e}");
                return ExitCode::FAILURE;
            }
        };
        let dat = Engine::new_with_filter_set(set).serialize();
        let mut check = Engine::default();
        if let Err(e) = std::fs::write(&args[2], &dat).map_err(|e| e.to_string()).and_then(|_| check.deserialize(&dat).map_err(|e| format!("{e:?}"))) {
            eprintln!("{}: {e}", args[2]);
            return ExitCode::FAILURE;
        }
        eprintln!("list engine {}: {:.1} MB", args[2], dat.len() as f64 / 1e6);
        return ExitCode::SUCCESS;
    }
    if args.len() < 6 {
        eprintln!("usage: wren-ubo-compile OUT_DIR WAR_DIR REDIRECT_RESOURCES_JS SCRIPTLETS_JSON LIST[:trusted][:hosts]...");
        return ExitCode::from(2);
    }
    let out = Path::new(&args[1]);
    let (war_dir, redirect_map, scriptlets) = (Path::new(&args[2]), Path::new(&args[3]), Path::new(&args[4]));

    let mut set = FilterSet::new(false);
    for spec in &args[5..] {
        let mut parts = spec.split(':');
        let path = parts.next().unwrap();
        let flags: Vec<&str> = parts.collect();
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) => {
                eprintln!("{path}: {e}");
                return ExitCode::FAILURE;
            }
        };
        let opts = ParseOptions {
            format: if flags.contains(&"hosts") { FilterFormat::Hosts } else { FilterFormat::Standard },
            permissions: if flags.contains(&"trusted") { TRUSTED } else { PermissionMask::default() },
            ..Default::default()
        };
        let lines = text.lines().count();
        set.add_filter_list(text, opts);
        eprintln!("list {path}: {lines} lines{}", if flags.is_empty() { String::new() } else { format!(" ({})", flags.join(", ")) });
    }

    let started = std::time::Instant::now();
    let engine = Engine::new_with_filter_set(set);
    let dat = engine.serialize();
    eprintln!("engine: built in {:.1} s, {:.1} MB serialized", started.elapsed().as_secs_f64(), dat.len() as f64 / 1e6);

    let mut resources = assemble_web_accessible_resources(war_dir, redirect_map);
    let redirects = resources.len();
    let scriptlet_json = match std::fs::read_to_string(scriptlets) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("{}: {e}", scriptlets.display());
            return ExitCode::FAILURE;
        }
    };
    let mut scriptlet_resources: Vec<Resource> = match serde_json::from_str(&scriptlet_json) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("{}: {e}", scriptlets.display());
            return ExitCode::FAILURE;
        }
    };
    let scriptlet_count = scriptlet_resources.len();
    resources.append(&mut scriptlet_resources);
    let resources_json = serde_json::to_string(&resources).expect("resources serialize");
    eprintln!("resources: {redirects} redirect targets, {scriptlet_count} scriptlets, {:.1} MB", resources_json.len() as f64 / 1e6);

    if let Err(e) = std::fs::create_dir_all(out)
        .and_then(|_| std::fs::write(out.join("engine.dat"), &dat))
        .and_then(|_| std::fs::write(out.join("resources.json"), &resources_json))
    {
        eprintln!("{}: {e}", out.display());
        return ExitCode::FAILURE;
    }

    // Reload as the TV does (wren_ubo_new) and check that the lists and resources do what they should.
    let started = std::time::Instant::now();
    let mut check = Engine::default();
    if let Err(e) = check.deserialize(&dat) {
        eprintln!("engine.dat does not load: {e:?}");
        return ExitCode::FAILURE;
    }
    check.use_resources(serde_json::from_str::<Vec<Resource>>(&resources_json).unwrap());
    eprintln!("reload: {:.0} ms", started.elapsed().as_secs_f64() * 1e3);
    let mut failed = false;
    let mut expect = |what: &str, ok: bool| {
        eprintln!("  {} {what}", if ok { "ok  " } else { "FAIL" });
        failed |= !ok;
    };
    let req = |url: &str, src: &str, ty: &str| Request::new(url, src, ty, "GET").unwrap();
    let r = check.check_network_request(&req("https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js", "https://example.com/", "script"));
    expect("adsbygoogle.js blocked", r.should_block());
    let r = check.check_network_request(&req("https://www.googletagmanager.com/gtm.js?id=GTM-XXXX", "https://example.com/", "script"));
    expect("gtm.js blocked or redirected", r.should_block());
    let r = check.check_network_request(&req("https://code.jquery.com/jquery-3.7.1.min.js", "https://example.com/", "script"));
    expect("jquery allowed", !r.should_block());
    let r = check.check_network_request(&req("https://www.google-analytics.com/analytics.js", "https://example.com/", "script"));
    expect("analytics.js redirected to a stub", r.should_block() && r.redirect.as_deref().is_some_and(|d| d.starts_with("data:")));
    let c = check.url_cosmetic_resources("https://www.example.com/");
    expect("generic cosmetic data reachable", !c.generichide);
    let generic = check.hidden_class_id_selectors(["ADBox"], ["ad_banner"], &c.exceptions);
    expect(&format!("generic hiding for .ADBox / #ad_banner ({generic:?})"), generic.len() >= 2);
    let scripts = check.url_cosmetic_resources("https://www.youtube.com/watch?v=x").injected_script;
    expect("scriptlets resolve on youtube.com", !scripts.is_empty());
    if failed { ExitCode::FAILURE } else { ExitCode::SUCCESS }
}
