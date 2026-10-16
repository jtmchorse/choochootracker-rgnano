#!/bin/sh
# ChooChooTracker launcher for the Anbernic RG Nano (DrUm78 FunKey-OS, GMenu2X OPK).
# Runs with the OPK mount as the working directory (read-only assets live here).
# User data, settings, autosave, exports and the log go to /mnt/ChooChooTracker.
cd "$(dirname "$0")" || exit 1
DATA=/mnt/ChooChooTracker
mkdir -p "$DATA" 2>/dev/null
export LD_LIBRARY_PATH="$PWD/libs:$LD_LIBRARY_PATH"
# Dummy video driver with evdev input: the app draws to /dev/fb0 itself.
export SDL_VIDEODRIVER=evdev
# The buttons are a uinput keyboard that registers as Name="ample" (fkgpiod
# passes the wrong struct, so "uinput-sample" is truncated). Match on the Name
# line only, because a USB DAC can shift the event node numbers.
DEV=$(awk '
/^N: Name=/ { m = ($0 ~ /Name="(uinput-s)?ample"/) }
/^H: Handlers=/ { if (m) { for (i = 1; i <= NF; i++) if ($i ~ /^event[0-9]+$/) { print $i; exit } } }
' /proc/bus/input/devices 2>/dev/null)
if [ -n "$DEV" ]; then
    export SDL_EVDEV_DEVICES="2:/dev/input/$DEV"
else
    L=""
    for f in /dev/input/event*; do [ -e "$f" ] && L="$L,2:$f"; done
    export SDL_EVDEV_DEVICES="${L#,}"
fi
echo "launcher: node=${DEV:-none} SDL_EVDEV_DEVICES=$SDL_EVDEV_DEVICES" > "$DATA/launcher.txt" 2>/dev/null
exec ./choochootracker > "$DATA/stderr.txt" 2>&1
