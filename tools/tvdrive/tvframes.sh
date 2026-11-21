#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# TV side of "tvdrive frames" (piped over ssh): sh -s SECONDS GAP_US. A burst of small screen captures with their times.
secs=$1; gap=$2
rm -rf /tmp/tvdrive-frames; mkdir -p /tmp/tvdrive-frames; cd /tmp/tvdrive-frames || exit 1
end=$(( $(cut -d. -f1 /proc/uptime) + secs )); i=0
while [ "$(cut -d. -f1 /proc/uptime)" -lt "$end" ]; do
  n=$(printf %05d $i)
  luna-send -n 1 luna://com.webos.service.capture/executeOneShot \
    "{\"path\":\"/tmp/tvdrive-frames/f$n.jpg\",\"method\":\"DISPLAY\",\"width\":320,\"height\":180,\"format\":\"JPG\"}" </dev/null >/dev/null 2>&1
  echo "$n $(cut -d' ' -f1 /proc/uptime)" >> times.txt
  i=$((i+1)); usleep "$gap"
done
tar cf /tmp/tvdrive-frames.tar -C /tmp/tvdrive-frames . && rm -rf /tmp/tvdrive-frames
