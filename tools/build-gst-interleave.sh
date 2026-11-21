#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# libgstinterleave.so (GStreamer's "interleave"/"deinterleave" elements) for the 32-bit engine, from the sources that are
# already in the Buildroot tree (gst-plugins-good), cross-compiled in the build container. WebKit's decodeAudioData
# builds "decodebin ! ... ! deinterleave"; without the element the web process crashed on the first page that decoded
# audio. A Buildroot rebuild would do the same (BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_INTERLEAVE), this does not need one.
# usage: tools/build-gst-interleave.sh BR_DIR OUT_DIR     (BR_DIR contains br-out/, the one the engine is built from)
set -euo pipefail
br=$(cd "${1:?BR_DIR}" && pwd); out=${2:?OUT_DIR}; mkdir -p "$out"; out=$(cd "$out" && pwd)
case "$out/" in "$br"/*) ;; *) echo "OUT_DIR must be inside BR_DIR (it is mounted into the container)" >&2; exit 1 ;; esac
rel=${out#"$br"/}
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build -v "$br:/build" -w /build webos-browser-build:1 sh -c '
    set -e
    export PATH=/build/br-out/host/bin:$PATH
    g=$(ls -d /build/br-out/build/gst1-plugins-good-*/ | head -n 1); g=${g%/}
    cd "$g/gst/interleave"
    arm-webos-linux-gnueabi-gcc -shared -fPIC -O2 -DHAVE_CONFIG_H -I"$g/buildroot-build" -I"$g/gst/interleave" -I"$g/gst-libs" \
        -I"$g/buildroot-build/gst-libs" plugin.c interleave.c deinterleave.c -o /build/'"$rel"'/libgstinterleave.so \
        $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0) -Wl,-soname,libgstinterleave.so'
ls -l "$out/libgstinterleave.so"
