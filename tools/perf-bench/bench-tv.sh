#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One-script benchmark of the DEV app on the TV (never the real app). Needs: ssh host "lgtv" (root, /opt/bin/tv, python3),
# node (motion.js), the shared TV lock (taken here, released at the end). Prints a table; every number is a plain "key=value".
#   bash tools/perf-bench/bench-tv.sh LABEL [RUNS=2]
# Results go to $OUT (default build/plan/build/bench/LABEL): summary.txt, per-run engine/events logs, the motion trace text.
# A measurement that is missing prints NA (never 0 or an empty value); the logs are removed before each launch so no figure is read from an earlier run.
# Per run: jsb (JS kernels, ms), 3 cold page loads (finish ms, web-process peak RSS), a 60 s self-scrolling page with the
# motion recorder (frame gaps), 28 s of a generic HLS test page (decoded / dropped frames). The TV must be quiet: the
# CPU idle percentage (top, 2 s; NOT the load average: this TV shows 14 at 93% idle) is printed first, the run is refused below IDLE_MIN.
# Only the dev app's session files and HTTP cache are cleared between runs; they are saved first and put back at the end.
set -u
LABEL=${1:?label}; RUNS=${2:-2}
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
OUT=${OUT:-$root/build/plan/build/bench/$LABEL}; mkdir -p "$OUT"
APP=org.sostk.wpebrowser.dev; LOCK=/tmp/wren-devapp.lock; OWNER=${OWNER:-wren-build}; PORT=18899
IDLE_MIN=${IDLE_MIN:-85}; PAGES=${PAGES:-"https://en.wikipedia.org/wiki/Romania https://www.bbc.com/news https://www.theguardian.com/international"}
ONLY=${ONLY:-"jsb pages scroll video"}  # which parts to run, e.g. ONLY=pages; PAGE_ENV='"WREN_UBO":"0"' adds wpebEnv entries to the page loads
penv=""; [ -n "${PAGE_ENV:-}" ] && penv=",\"wpebEnv\":{$PAGE_ENV}"
want() { case " $ONLY " in *" $1 "*) return 0 ;; esac; return 1; }
SUM=$OUT/summary.txt; : > "$SUM"
tv() { timeout "${1:-45}" ssh -o BatchMode=yes -o ConnectTimeout=25 lgtv "$2"; }
say() { echo "$*" | tee -a "$SUM"; }
na() { if [ -n "$1" ]; then printf '%s' "$1"; else printf NA; fi; }  # a measurement that is missing prints NA, never 0 or an empty value
RT='export PATH=/opt/bin:/opt/sbin:$PATH'

# the lock: taken here, or already held by $OWNER (a caller that ran something else first); released at the end either way
if [ "$(tv 20 "head -1 $LOCK/owner 2>/dev/null")" != "$OWNER" ] && ! tv 30 "mkdir $LOCK 2>/dev/null && { { printf '%s\n%s\n' '$OWNER' \$(date +%s) > $LOCK/owner.tmp && mv $LOCK/owner.tmp $LOCK/owner; } || { rm -rf $LOCK; exit 1; }; }"; then echo "TV lock held by: $(tv 20 "head -1 $LOCK/owner 2>/dev/null")"; exit 2; fi
DATA=$(tv 30 "for d in /media/developer/apps/usr/palm/applications/$APP/$APP.data*; do [ -d \$d/webkit ] && { echo \$d; break; }; done")
cleanup() {
  tv 40 "$RT; tv close $APP >/dev/null 2>&1; sleep 1; [ -f /tmp/wrenbench/srv.pid ] && kill \$(cat /tmp/wrenbench/srv.pid) 2>/dev/null
    if [ -n '$DATA' ] && [ -d '$DATA.bench-bak' ]; then rm -rf '$DATA/profile'; mv '$DATA.bench-bak' '$DATA/profile'; fi
    if [ -d '$DATA/webkit/cache.bench-bak' ]; then rm -rf '$DATA/webkit/cache'; mv '$DATA/webkit/cache.bench-bak' '$DATA/webkit/cache'; fi
    rm -rf /tmp/wrenbench
    if [ \$(head -n1 $LOCK/owner 2>/dev/null) = '$OWNER' ]; then rm -f $LOCK/owner; rmdir $LOCK; echo lock released; fi" || echo "WARNING: cleanup failed, the lock may still be held by $OWNER"
}
trap cleanup EXIT

idle() { tv 30 "top -bn2 -d 2 | grep '^%Cpu' | tail -1 | sed 's/.* \([0-9.]*\) id.*/\1/'"; }
load=$(idle)
say "== $LABEL $(date +%FT%T) cpu_idle=$load% runs=$RUNS app=$APP"
say "engine: $(tv 20 "ls -l /media/developer/apps/usr/palm/applications/$APP/engine/wpe-engine32 /media/developer/apps/usr/palm/applications/$APP/engine/lib/libWPEWebKit-2.0.so.1 | awk '{print \$5, \$9}' | tr '\n' ';'")"
for i in 1 2 3 4 5 6; do awk -v l="$load" -v m="$IDLE_MIN" 'BEGIN{exit !(l>=m)}' && break; sleep 15; load=$(idle); done
awk -v l="$load" -v m="$IDLE_MIN" 'BEGIN{exit !(l>=m)}' || { say "TV NOT QUIET (cpu idle $load% < $IDLE_MIN%): refusing to measure"; exit 3; }

if [ -n "$DATA" ]; then tv 40 "$RT; tv close $APP >/dev/null 2>&1; sleep 1; [ -d '$DATA/profile' ] && cp -a '$DATA/profile' '$DATA.bench-bak'; [ -d '$DATA/webkit/cache' ] && mv '$DATA/webkit/cache' '$DATA/webkit/cache.bench-bak'; true"; fi
tv 30 "mkdir -p /tmp/wrenbench"
timeout 60 scp -q -o BatchMode=yes "$here/web/"* "$root/tools/tvdrive/motion-recorder.js" lgtv:/tmp/wrenbench/
tv 30 "cd /tmp/wrenbench && (nohup /opt/bin/python3 srv.py $PORT >srv.log 2>&1 </dev/null & echo \$! > srv.pid); sleep 1; rm -f results.log"

clean_launch() {  # clean_launch JSON: close the app, clear the session and the HTTP cache, launch
  tv 40 "$RT; tv close $APP >/dev/null 2>&1; sleep 2
    if [ -n '$DATA' ]; then rm -f '$DATA'/profile/session*; rm -rf '$DATA/webkit/cache'; mkdir -p '$DATA/webkit/cache'; chown -R \$(stat -c %u:%g '$DATA') '$DATA/webkit/cache' 2>/dev/null; fi
    : > /tmp/wrenbench/results.log; rm -f /tmp/$APP/engine.log /tmp/$APP/events.log    tv launch $APP '$1' >/dev/null"
}
check_env() {  # check_env EVENTS_LOG: every PAGE_ENV name must appear as "engine env NAME=" (the engine launch logs each name it accepts); a name the browser drops would void the run
  [ -n "${PAGE_ENV:-}" ] || return 0
  local n
  for n in $(printf '%s' "$PAGE_ENV" | grep -o '"[A-Za-z0-9_]*"[[:space:]]*:' | tr -d '": \t'); do
    grep -q "engine env $n=" "$1" || { say "FAIL: PAGE_ENV name $n was not accepted by the browser (no 'engine env $n=' in $1): this run does not measure what was asked"; exit 4; }
  done
}
savelogs() {  # savelogs TAG
  tv 30 "d=/tmp/$APP; cp \$d/engine.log /tmp/wrenbench/e.log; cp \$d/events.log /tmp/wrenbench/v.log" >/dev/null 2>&1
  timeout 60 scp -q -o BatchMode=yes lgtv:/tmp/wrenbench/e.log "$OUT/engine-$1.log"; timeout 60 scp -q -o BatchMode=yes lgtv:/tmp/wrenbench/v.log "$OUT/events-$1.log"
}
webrss() { tv 15 "for x in /proc/[0-9]*; do if grep -q WPEWebProcess \$x/comm 2>/dev/null; then awk '/VmRSS/{print \$2}' \$x/status; break; fi; done"; }

want jsb && for r in $(seq 1 "$RUNS"); do
  clean_launch "{\"target\":\"http://127.0.0.1:$PORT/jsb.html?run=$r\"}"
  res=""; for i in $(seq 1 40); do sleep 1; res=$(tv 15 "grep 'run=$r' /tmp/wrenbench/results.log | tail -1"); [ -n "$res" ] && break; done
  say "jsb run=$r $(na "$(echo "$res" | sed 's/.*\t//')")"
done
want pages && for url in $PAGES; do for r in $(seq 1 "$RUNS"); do
  tag="page-${url//[^a-z0-9]/_}-$r"
  clean_launch "{\"target\":\"$url\"$penv}"; peak=0; fin=""
  for i in $(seq 1 60); do sleep 1; rs=$(webrss); [ -n "$rs" ] && [ "$rs" -gt "$peak" ] && peak=$rs
    [ -z "$fin" ] && fin=$(tv 15 "grep -m1 'load finished after' /tmp/$APP/engine.log" 2>/dev/null | sed -n 's/.*after \([0-9]*\) ms.*/\1/p')
    [ -n "$fin" ] && [ "$i" -ge 25 ] && break; done
  savelogs "$tag"
  check_env "$OUT/events-$tag.log"
  say "page url=$url run=$r finish_ms=${fin:-NA} web_rss_peak_mb=$([ "$peak" -gt 0 ] && echo $((peak/1024)) || echo NA) commit_ms=$(na "$(grep -m1 'load committed after' "$OUT/engine-$tag.log" 2>/dev/null | sed -n 's/.*after \([0-9]*\) ms.*/\1/p')")"
done; done
want scroll && for r in $(seq 1 "$RUNS"); do
  clean_launch "{\"target\":\"http://127.0.0.1:$PORT/scroll.html?px=6&s=60\",\"wpebEnv\":{\"WPE_BROWSER_DEBUG_SCRIPT\":\"/tmp/wrenbench/motion-recorder.js\",\"WPE_BROWSER_DEBUG_CONSOLE\":\"1\"}}"
  for i in $(seq 1 30); do sleep 3; tv 15 "grep -q 'scroll px' /tmp/wrenbench/results.log" && break; done
  savelogs "scroll-$r"; own=$(tv 15 "grep 'scroll px' /tmp/wrenbench/results.log | tail -1")
  node "$root/tools/tvdrive/motion.js" "$OUT/engine-scroll-$r.log" > "$OUT/motion-scroll-$r.txt" 2>&1
  say "scroll run=$r $(na "$(grep -m1 '^frame gap' "$OUT/motion-scroll-$r.txt")") | page: $(na "$own")"
done
want video && for r in $(seq 1 "$RUNS"); do
  clean_launch "{\"target\":\"http://127.0.0.1:$PORT/fstest.html?autoplay=1\",\"wpebEnv\":{\"WPE_BROWSER_DEBUG_ENGINE\":\"32\",\"WPE_BROWSER_DEBUG_CONSOLE\":\"1\",\"WPE_BROWSER_DEBUG_AUTOPLAY\":\"allow\"}}"
  sleep 28; savelogs "video-$r"
  last=$(grep 'hybrid video: {"quality"' "$OUT/engine-video-$r.log" 2>/dev/null | tail -1 | sed 's/.*hybrid video: //')
  ff=$(grep -m1 'first frame presented' "$OUT/events-video-$r.log" 2>/dev/null | awk '{print $1}')
  say "video run=$r last_quality=$(na "$last") first_frame=$([ -n "$ff" ] && echo "${ff}ms" || echo NA) sink=$(na "$(grep 'sink:' "$OUT/events-video-$r.log" 2>/dev/null | tail -1 | sed 's/.*sink: //')")"
done
say "== end cpu_idle=$(idle)%"
