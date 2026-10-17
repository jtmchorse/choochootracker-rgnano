#!/bin/sh
# rgnano-bench2: CPU cost battery for the ChooChooTracker engines and effects.
# Renders offline at 32 kHz (no audio device is opened) and writes a table to
# /mnt/ChooChooTracker/bench.txt, then exits back to the launcher.
# busybox-ash compatible.
cd "$(dirname "$0")" || exit 1
DATA=/mnt/ChooChooTracker
mkdir -p "$DATA" 2>/dev/null
[ -w "$DATA" ] || DATA=/tmp/ChooChooTracker
mkdir -p "$DATA" 2>/dev/null
export RGNANO_DATA_DIR="$DATA"
# Keep the previous result.
[ -f "$DATA/bench.txt" ] && mv -f "$DATA/bench.txt" "$DATA/bench.prev.txt"
./bench_device --assets . --est bench-est.tsv --log "$DATA/bench.txt" > "$DATA/bench-stdout.txt" 2>&1
RC=$?
echo "launcher: bench_device exited rc=$RC" >> "$DATA/bench.txt"
sync
exit "$RC"
