#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Performance benchmark on the TV (ares device "tv" unless ARES_DEVICE is set). Uses the installed Wren; changes
# nothing on the TV except starting and closing Wren and a test server in /tmp/emetest (removed at the end).
#   tools/perf-bench/run.sh LABEL [RUNS]
# For each site: a cold start of Wren with that URL (app start + page load), with the web process's CPU time and
# memory once the page has loaded; then the scroll benchmark page; then the UI process's CPU use over 10 s on a still
# page. Results: build/perf-bench/LABEL.txt.
#   WPEB_ENV='{"JSC_thresholdForJITAfterWarmUp":"100"}'   engine environment for every launch (launch param wpebEnv)
#   SITES="https://a https://b"   other sites;   NO_SCROLL=1   only the page loads
set -euo pipefail
label=${1:?label}; runs=${2:-3}
dev=${ARES_DEVICE:-tv}
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
out="$root/build/perf-bench"; mkdir -p "$out"; rep="$out/$label.txt"; : > "$rep"
read -r -a sites <<< "${SITES:-https://en.wikipedia.org/wiki/Wren https://www.bbc.com/news https://github.com/about}"
env_json=${WPEB_ENV:+,\"wpebEnv\":$WPEB_ENV}
app=org.sostk.wpebrowser; logs=/tmp/org.sostk.wpebrowser
sh_tv() { ares-shell -d "$dev" -r "$1" 2>/dev/null; }
say() { echo "$*" | tee -a "$rep"; }

wait_for() {  # wait_for FILE PATTERN TRIES: until PATTERN appears in FILE on the TV
    local i
    for ((i = 0; i < $3; i++)); do
        sh_tv "grep -q '$2' $1" && return 0
    done
    return 1
}

launch() {  # a cold start with this URL; waits until the engine log has started fresh
    ares-launch -d "$dev" --close "$app" >/dev/null 2>&1 || true
    ares-launch -d "$dev" "$app" -p "{\"target\":\"$1\"$env_json}" >/dev/null
    local i
    for ((i = 0; i < 40; i++)); do sh_tv "! grep -q 'tab 1 load finished' $logs/engine.log" && return 0; done
}

say "== $label  $(date -u +%FT%TZ)  $(git -C "$root" rev-parse --short HEAD)  env ${WPEB_ENV:-none}"
for site in "${sites[@]}"; do
    for ((r = 1; r <= runs; r++)); do
        launch "$site"
        if wait_for "$logs/engine.log" 'tab 1 timing:' 120; then
            t=$(sh_tv "grep -m1 'tab 1 timing:' $logs/engine.log | sed 's/.*timing: //'")
            ev=$(sh_tv "awk '/first frame/{f=\$1} /engine ready/{e=\$1} /tab 1: page surface/{p=\$1} END{print \"first-frame \" f \" engine-ready \" e \" page-shown \" p}' $logs/events.log")
            # Web process CPU (utime+stime ticks, 100 Hz) since it started, and its memory, once the page has loaded.
            wp=$(sh_tv "for p in \$(pgrep -f '[l]ibexec/WPEWebProcess'); do awk '{c+=\$14+\$15} END{printf \"cpu %d \", c}' /proc/\$p/stat; awk '/VmRSS/{printf \"rss %dMB \", \$2/1024}' /proc/\$p/status; done")
            say "$site run $r | $ev | webproc ${wp:-?}| $t"
        else
            say "$site run $r | no timing within the wait"
        fi
    done
done

[ -n "${NO_SCROLL:-}" ] && { ares-launch -d "$dev" --close "$app" >/dev/null 2>&1 || true; say "== done"; exit 0; }
sh_tv "mkdir -p /tmp/emetest"
ares-push -d "$dev" "$root/build/minihttpd" /tmp/emetest/ >/dev/null
ares-push -d "$dev" "$here/scroll.html" /tmp/emetest/ >/dev/null
sh_tv "cd /tmp/emetest && chmod +x minihttpd && rm -f report.txt && (setsid ./minihttpd 8099 /tmp/emetest /tmp/emetest/report.txt >/dev/null 2>&1 &)"
for ((r = 1; r <= runs; r++)); do
    sh_tv "rm -f /tmp/emetest/report.txt"
    launch "http://127.0.0.1:8099/scroll.html?px=12&s=6"
    wait_for /tmp/emetest/report.txt ended 200 || true
    say "scroll run $r | $(sh_tv "grep '^.*scroll ' /tmp/emetest/report.txt | sed 's/^[^s]*scroll/scroll/'")"
done

# The UI process's CPU over 10 s on the still benchmark page (utime+stime ticks, 100 Hz).
cpu=$(sh_tv "p=\$(pgrep -f '$app/wpebrowser' | head -1); a=\$(awk '{print \$14+\$15}' /proc/\$p/stat); sleep 10; b=\$(awk '{print \$14+\$15}' /proc/\$p/stat); echo \$((b-a))")
say "ui-cpu still page: ${cpu:-?} ticks / 10 s (100 ticks = one core for 1 s)"

ares-launch -d "$dev" --close "$app" >/dev/null 2>&1 || true
sh_tv "pkill minihttpd; rm -rf /tmp/emetest" || true
say "== done"
