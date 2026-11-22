#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# TV side of "tvdrive frames" (piped over ssh): sh -s SECONDS GAP_US. A burst of small screen captures with their times.
# What the capture service answers when it writes no file goes to errors.txt (the first few), so the PC can say why.
secs=$1; gap=$2; tag=${3:-x}   # tag: the owner's name, so two owners never share a folder
fd=/tmp/tvdrive-frames-$tag
rm -rf "$fd"; mkdir -p "$fd"; cd "$fd" || exit 1
end=$(( $(cut -d. -f1 /proc/uptime) + secs )); i=0; bad=0
while [ "$(cut -d. -f1 /proc/uptime)" -lt "$end" ]; do
  n=$(printf %05d $i)
  r=$(luna-send -n 1 luna://com.webos.service.capture/executeOneShot \
    "{\"path\":\"$fd/f$n.jpg\",\"method\":\"DISPLAY\",\"width\":320,\"height\":180,\"format\":\"JPG\"}" </dev/null 2>&1)
  if [ ! -s "f$n.jpg" ] && [ "$bad" -lt 3 ]; then bad=$((bad+1)); echo "f$n: $r" | tr '\n' ' ' >> errors.txt; echo >> errors.txt; fi
  [ -s "f$n.jpg" ] && echo "$n $(cut -d' ' -f1 /proc/uptime)" >> times.txt
  i=$((i+1)); usleep "$gap"
done
tar cf "$fd.tar" -C "$fd" . && rm -rf "$fd"
