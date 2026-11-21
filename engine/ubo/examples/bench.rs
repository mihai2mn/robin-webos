// SPDX-License-Identifier: GPL-3.0-or-later
//! Host benchmark and decision digest for the engine, on the data the TV ships.
//!
//!   cargo run --release --example bench -- DATA_DIR CORPUS.tsv COSMETIC_URLS.txt [ROUNDS]
//!
//! DATA_DIR is the package's engine/ubo (engine.dat, resources.json, lists/*.dat, lists/catalog.ini).
//! CORPUS.tsv: one request per line, "url<TAB>source page<TAB>type". COSMETIC_URLS.txt: one document URL per line.
//! Prints load time and memory (core, then the lists catalog.ini turns on by default), per-request and per-document
//! cost, and a digest of every decision: the digest must not change when only performance work is done.
use std::collections::hash_map::DefaultHasher;
use std::collections::BTreeSet;
use std::ffi::{c_char, CStr, CString};
use std::hash::{Hash, Hasher};
use std::time::Instant;
use wren_ubo::*;

fn rss_kb() -> (u64, u64) {
    let s = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
    let get = |k: &str| s.lines().find(|l| l.starts_with(k)).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok()).unwrap_or(0);
    (get("VmRSS:"), get("VmHWM:"))
}

fn take(p: *mut c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
    wren_ubo_string_free(p);
    s
}

/// A JSON text with arrays of strings sorted, so hash-set iteration order does not show.
fn canon(json: &str) -> String {
    fn sort(v: &mut serde_json::Value) {
        match v {
            serde_json::Value::Array(a) => {
                a.iter_mut().for_each(sort);
                a.sort_by_key(|x| x.to_string());
            }
            serde_json::Value::Object(o) => o.values_mut().for_each(sort),
            _ => {}
        }
    }
    let mut v: serde_json::Value = serde_json::from_str(json).unwrap_or(serde_json::Value::Null);
    sort(&mut v);
    v.to_string()
}

fn sorted_lines(s: &str) -> String {
    s.lines().collect::<BTreeSet<_>>().into_iter().collect::<Vec<_>>().join("\n")
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: bench DATA_DIR CORPUS.tsv COSMETIC_URLS.txt [ROUNDS]");
        std::process::exit(2);
    }
    let dir = std::path::Path::new(&a[1]);
    let rounds: usize = a.get(4).and_then(|r| r.parse().ok()).unwrap_or(5);
    let (r0, _) = rss_kb();

    let t = Instant::now();
    let dat = std::fs::read(dir.join("engine.dat")).unwrap();
    let res = CString::new(std::fs::read_to_string(dir.join("resources.json")).unwrap()).unwrap();
    println!("read engine.dat ({:.1} MB) + resources.json: {:.0} ms", dat.len() as f64 / 1e6, t.elapsed().as_secs_f64() * 1e3);
    let t = Instant::now();
    let ubo = wren_ubo_new(dat.as_ptr(), dat.len(), res.as_ptr());
    assert!(!ubo.is_null());
    println!("wren_ubo_new (core): {:.0} ms", t.elapsed().as_secs_f64() * 1e3);
    drop(dat);
    let (r1, h1) = rss_kb();
    println!("RSS: start {r0} kB, after core {r1} kB (+{} kB), peak {h1} kB", r1 as i64 - r0 as i64);

    // The lists the catalog turns on by default.
    let catalog = std::fs::read_to_string(dir.join("lists/catalog.ini")).unwrap_or_default();
    let (mut name, mut defaults) = (String::new(), Vec::new());
    for l in catalog.lines() {
        if let Some(n) = l.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            name = n.to_string();
        } else if l == "default=true" {
            defaults.push(name.clone());
        }
    }
    let t = Instant::now();
    for n in &defaults {
        let d = std::fs::read(dir.join("lists").join(format!("{n}.dat"))).unwrap();
        let c = CString::new(n.as_str()).unwrap();
        assert_eq!(wren_ubo_add_list(ubo as *mut _, c.as_ptr(), d.as_ptr(), d.len()), 1);
    }
    let (r2, h2) = rss_kb();
    println!("default lists {:?}: {:.0} ms; RSS {r2} kB (+{} kB), peak {h2} kB", defaults, t.elapsed().as_secs_f64() * 1e3, r2 as i64 - r1 as i64);

    let corpus: Vec<(CString, CString, CString)> = std::fs::read_to_string(&a[2])
        .unwrap()
        .lines()
        .filter_map(|l| {
            let mut f = l.split('\t');
            Some((CString::new(f.next()?).ok()?, CString::new(f.next()?).ok()?, CString::new(f.next()?).ok()?))
        })
        .collect();
    let mut best = f64::MAX;
    let mut digest = DefaultHasher::new();
    let mut counts = [0usize; 4];
    for round in 0..rounds {
        let t = Instant::now();
        for (u, s, ty) in &corpus {
            let v = wren_ubo_check(ubo, u.as_ptr(), s.as_ptr(), ty.as_ptr());
            if round == 0 {
                counts[v.action as usize] += 1;
                v.action.hash(&mut digest);
                take(v.text).hash(&mut digest);
            } else {
                take(v.text);
            }
        }
        best = best.min(t.elapsed().as_secs_f64());
    }
    println!(
        "check: {} requests, best of {rounds}: {:.1} us/request; allow {} block {} redirect {} rewrite {}",
        corpus.len(), best * 1e6 / corpus.len() as f64, counts[0], counts[1], counts[2], counts[3]
    );
    println!("check digest: {:016x}", digest.finish());

    let pages: Vec<CString> = std::fs::read_to_string(&a[3]).unwrap().lines().map(|l| CString::new(l).unwrap()).collect();
    let classes = CString::new("ad\nads\nadvert\nbanner\nad-slot\nsponsored\ntop-bar\ncontent\nfooter\npromo\nwidget\ncookie-banner\npopup\nmodal").unwrap();
    let ids = CString::new("ad\nads\nbanner\nheader\nmain\nsidebar\ncookie\nconsent\npopup").unwrap();
    let mut best = f64::MAX;
    let mut digests: Vec<DefaultHasher> = (0..5).map(|_| DefaultHasher::new()).collect();
    let (mut css_len, mut script_len) = (0usize, 0usize);
    for round in 0..rounds {
        let t = Instant::now();
        for p in &pages {
            let mut c = wren_ubo_cosmetic(ubo, p.as_ptr());
            let hid = wren_ubo_hidden(ubo, classes.as_ptr(), ids.as_ptr(), c.exceptions);
            if round == 0 {
                let css = unsafe { CStr::from_ptr(c.css) }.to_string_lossy().into_owned();
                let script = unsafe { CStr::from_ptr(c.script) }.to_string_lossy().into_owned();
                css_len += css.len();
                script_len += script.len();
                // The order of selectors and scriptlets comes from hash sets (random per process): compare as sets of lines.
                sorted_lines(&css).hash(&mut digests[0]);
                sorted_lines(&script).hash(&mut digests[1]);
                canon(&unsafe { CStr::from_ptr(c.procedural) }.to_string_lossy()).hash(&mut digests[2]);
                canon(&unsafe { CStr::from_ptr(c.exceptions) }.to_string_lossy()).hash(&mut digests[3]);
                c.generichide.hash(&mut digests[3]);
                sorted_lines(&take(hid)).hash(&mut digests[4]);
            } else {
                take(hid);
            }
            wren_ubo_cosmetic_free(&mut c);
        }
        best = best.min(t.elapsed().as_secs_f64());
    }
    println!(
        "cosmetic+hidden: {} documents, best of {rounds}: {:.1} us/document; css {} B, scripts {} B in total",
        pages.len(), best * 1e6 / pages.len() as f64, css_len, script_len
    );
    let d: Vec<String> = digests.iter().map(|h| format!("{:016x}", h.finish())).collect();
    println!("cosmetic digest: css {} script {} procedural {} exceptions {} hidden {}", d[0], d[1], d[2], d[3], d[4]);
    let (r3, h3) = rss_kb();
    println!("RSS at end: {r3} kB, peak {h3} kB");
    wren_ubo_free(ubo as *mut _);
}
