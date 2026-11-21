#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The laptop (ssh host "vantop": Linux, 32 threads, 62 GB, no docker, no root) as a second WebKit builder, WITHOUT root.
# The PC's build container (webos-browser-build:1) is exported once to a directory on the laptop and entered with bubblewrap
# (unprivileged user namespaces; the laptop has them on): same compilers, same absolute paths (/build), same results.
# Everything lives under $LBASE (default ~/wren-laptop-build) on the laptop; nothing else on the laptop (~/.cargo, sccache, /mnt/Data/...) is touched.
#
#   bash tools/laptop-builder.sh setup        PC -> laptop: the container root (0.7 GB), x-tools + br-out/host + headers + the
#                                             WebKit tarball (1.3 GB), and ~/wren-laptop-build/enter.sh   (about 1 minute)
#   bash tools/laptop-builder.sh tree         PC -> laptop: the existing WebKit build tree (6.2 GB) to build/br-out/build/
#   bash tools/laptop-builder.sh sync         the repo's tools/ and buildroot-external/patches to ~/wren-laptop-build/repo
#   bash tools/laptop-builder.sh build NAME [VAR=value...]   run tools/build-wk-variant.sh on the laptop (log: ~/wren-laptop-build/NAME.log)
#   bash tools/laptop-builder.sh fetch NAME   the variant's output (libs, helpers) back to $OUT_LOCAL (default ./variants-out/NAME)
#   bash tools/laptop-builder.sh enter [cmd]  a shell (or a command) inside the build root
# Host: WSL on the PC (docker, ssh to vantop). Needs nothing as root on either side.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
BR=${BUILD_ROOT:-$HOME/webos-browser-build}; L=${LAPTOP:-vantop}; D=${LBASE:-\$HOME/wren-laptop-build}   # LBASE: where on the laptop (an absolute path)
rs() { ssh -o BatchMode=yes "$L" "bash -c '$1'"; }     # the laptop's login shell is fish: always go through bash
case "${1:-}" in
  setup)
    cid=$(docker create webos-browser-build:1)
    docker export "$cid" | rs "mkdir -p $D/rootfs $D/build; tar x -C $D/rootfs --no-same-owner 2>/dev/null; true"
    docker rm "$cid" >/dev/null
    ( cd "$BR" && tar c x-tools br-out/host br-out/staging inc254 dl/wpewebkit ) | rs "tar x -C $D/build --no-same-owner"
    cat <<'EOS' | rs "cat > $D/enter.sh; chmod +x $D/enter.sh"
#!/bin/bash
# A command in the build container's root (exported from the PC's webos-browser-build:1), the inputs at /build, as the normal
# user (bubblewrap, user namespaces): ~/wren-laptop-build/enter.sh [-w DIR] [--bind HOST:CONTAINER]... command args...
D=$(cd "$(dirname "$0")" && pwd)
extra=(); cwd=/build
while [ $# -gt 0 ]; do case "$1" in -w) cwd=$2; shift 2 ;; --bind) extra+=(--bind "${2%%:*}" "${2#*:}"); shift 2 ;; *) break ;; esac; done
exec bwrap --bind "$D/rootfs" / --bind "$D/build" /build --dev /dev --proc /proc --tmpfs /tmp --bind /sys /sys \
  --unshare-pid --die-with-parent --chdir "$cwd" --setenv HOME /build --setenv PATH /build/br-out/host/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin "${extra[@]}" "$@"
EOS
    rs "$D/enter.sh cmake --version | head -1; $D/enter.sh /build/br-out/host/bin/arm-webos-linux-gnueabi-gcc --version | head -1" ;;
  tree)
    ( cd "$BR" && tar c br-out/build/wpewebkit-2.54.3 ) | rs "tar x -C $D/build --no-same-owner" ;;
  sync)
    ( cd "$repo" && tar c tools/build-wk-variant.sh buildroot-external/patches/wpewebkit/2.54.3 ) | rs "chmod -R u+w $D/repo 2>/dev/null; rm -rf $D/repo; mkdir -p $D/repo; tar x -C $D/repo --no-same-permissions --no-same-owner" ;;
  build)
    name=${2:?name}; shift 2
    printf 'export %q\n' "$@" | rs "cat > $D/$name.env"      # the variables (flags with spaces) travel in a file
    rs "cd $D/repo && . $D/$name.env && ENTER=$D/enter.sh BUILD_ROOT=$D/build VARIANTS=$D/variants NAME=$name nohup bash tools/build-wk-variant.sh all > $D/$name.log 2>&1 < /dev/null &" ;;
  fetch)
    name=${2:?name}; out=${OUT_LOCAL:-./variants-out/$name}; mkdir -p "$out"
    rs "tar c -C $D/variants/$name-out ." | tar x -C "$out" ;;
  enter) shift; ssh -o BatchMode=yes -t "$L" $D/enter.sh ${@:-bash} ;;
  *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
