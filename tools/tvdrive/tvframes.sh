#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# TV side of "tvdrive frames" (piped over ssh): sh -s SECONDS GAP_US. A burst of small screen captures with their times.
# What the capture service answers when it writes no file goes to errors.txt (the first few), so the PC can say why.
secs=$1; gap=$2
rm -rf /tmp/tvdrive-frames; mkdir -p /tmp/tvdrive-frames; cd /tmp/tvdrive-frames || exit 1
end=$(( $(cut -d. -f1 /proc/uptime) + secs )); i=0; bad=0
while [ "$(cut -d. -f1 /proc/uptime)" -lt "$end" ]; do
  n=$(printf %05d $i)
  r=$(luna-send -n 1 luna://com.webos.service.capture/executeOneShot \
    "{\"path\":\"/tmp/tvdrive-frames/f$n.jpg\",\"method\":\"DISPLAY\",\"width\":320,\"height\":180,\"format\":\"JPG\"}" </dev/null 2>&1)
  if [ ! -s "f$n.jpg" ] && [ "$bad" -lt 3 ]; then bad=$((bad+1)); echo "f$n: $r" | tr '\n' ' ' >> errors.txt; echo >> errors.txt; fi
  [ -s "f$n.jpg" ] && echo "$n $(cut -d' ' -f1 /proc/uptime)" >> times.txt
  i=$((i+1)); usleep "$gap"
done
tar cf /tmp/tvdrive-frames.tar -C /tmp/tvdrive-frames . && rm -rf /tmp/tvdrive-frames
