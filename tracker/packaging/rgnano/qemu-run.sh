#!/bin/bash
# Boot the built OPK contents under qemu-arm-static (run inside the rgnano-build
# container). No audio hardware and no evdev: SDL's dummy audio, the frame goes
# to a dump file instead of /dev/fb0. Usage: qemu-run.sh [seconds] [canvas WxH]
#   output dir: $OUT (default /root/work/r4)
#     fb.raw  240x240 RGB565 little-endian, written by RGNANO_FB_DUMP
#     fb.ppm  the same frame as P6, refreshed every 2 s (RGNANO_FB_PPM)
#     data/   the /mnt/ChooChooTracker stand-in (settings, projects, rgnano-log.txt)
#     hwm.txt qemu process VmHWM samples every 2 s (qemu-user reports ITS OWN
#             process: it includes qemu's ~9 MB base and translation cache, so it
#             is an upper bound for the guest)
set -u
SECS=${1:-20}
CANVAS=${2:-}
OUT=${OUT:-/root/work/r4}
OPK=${OPK:-/root/work/ccr/releases/choochootracker-rgnano.opk}
SDK=/opt/funkey-sdk/FunKey-sdk-2.3.0
SYS=$SDK/arm-funkey-linux-musleabihf/sysroot
SDL=${RGNANO_SDL2_ROOT:-/root/work/stage-r3}

rm -rf "$OUT"
mkdir -p "$OUT"
unsquashfs -no-progress -d "$OUT/app" "$OPK" > /dev/null
cd "$OUT/app" || exit 1
export SDL_VIDEODRIVER=evdev SDL_AUDIODRIVER=dummy
export RGNANO_DATA_DIR="$OUT/data" RGNANO_FB_DUMP="$OUT/fb.raw" RGNANO_FB_PPM="$OUT/fb.ppm" RGNANO_FB_PPM_SECS=2
[ -n "$CANVAS" ] && export RGNANO_CANVAS="$CANVAS"
LIBS=$OUT/app/libs:$SYS/usr/lib:$SYS/lib
touch "$OUT/stamp"
qemu-arm-static "$SYS/lib/libc.so" --library-path "$LIBS" ./choochootracker > "$OUT/stdout.txt" 2> "$OUT/stderr.txt" &
PID=$!
: > "$OUT/hwm.txt"
END=$((SECONDS + SECS))
while [ $SECONDS -lt $END ] && kill -0 $PID 2>/dev/null; do
  grep -E 'VmHWM|VmRSS' /proc/$PID/status 2>/dev/null | tr '\n' ' ' >> "$OUT/hwm.txt"
  echo >> "$OUT/hwm.txt"
  sleep 2
done
if kill -0 $PID 2>/dev/null; then
  echo "STILL RUNNING after ${SECS}s (good); sending SIGTERM" | tee "$OUT/result.txt"
  kill -TERM $PID
  wait $PID 2>/dev/null
else
  wait $PID
  echo "EXITED EARLY with status $?" | tee "$OUT/result.txt"
fi
# Write-leak check: nothing may have been created or modified in the (read-only
# on the device) app directory while the app ran.
find "$OUT/app" -type f -newer "$OUT/stamp" > "$OUT/app-writes.txt"
echo "files written inside the app dir: $(wc -l < "$OUT/app-writes.txt")" | tee -a "$OUT/result.txt"
