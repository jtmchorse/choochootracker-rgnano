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

# fkgpiod key map: load ours (no FN combos) while the app runs, and put the
# system map back afterwards, however the app ends. The firmware's own keymap
# script is preferred (it handles the FIFO and logging); without it the command
# goes straight into the FIFO. Both block without a reader, so the work runs in
# a background job that is killed after ~3 s and the launcher can never hang.
FIFO=/tmp/fkgpiod.fifo
KEYMAP=/usr/local/sbin/keymap
MAP="$PWD/choochootracker.key"
LOG="$DATA/launcher.txt"
fifo_write() { echo "$1" > "$FIFO"; }
fk_send() { # $1 = keymap sub-command (load|default), $2 = daemon line, $3.. = keymap args
    sub=$1; line=$2; shift 2
    if [ ! -p "$FIFO" ]; then
        echo "launcher: fkgpiod: no FIFO, skipped '$line'" >> "$LOG" 2>/dev/null
        return 1
    fi
    if ! pidof fkgpiod > /dev/null 2>&1; then
        echo "launcher: fkgpiod: not running, skipped '$line'" >> "$LOG" 2>/dev/null
        return 1
    fi
    if [ -x "$KEYMAP" ]; then
        ( "$KEYMAP" "$sub" "$@" ) > /dev/null 2>&1 &
        how="keymap $sub"
    else
        ( fifo_write "$line" ) > /dev/null 2>&1 &
        how="fifo"
    fi
    w=$!
    n=0
    while [ "$n" -lt 15 ] && kill -0 "$w" 2> /dev/null; do
        sleep 0.2 2> /dev/null || sleep 1
        n=$((n + 1))
    done
    if kill -0 "$w" 2> /dev/null; then
        kill "$w" 2> /dev/null
        echo "launcher: fkgpiod: $how timed out (no reader), '$line'" >> "$LOG" 2>/dev/null
        return 1
    fi
    echo "launcher: fkgpiod: sent '$line' via $how" >> "$LOG" 2>/dev/null
    return 0
}
RESTORED=0
restore_map() {
    [ "$RESTORED" = 1 ] && return
    RESTORED=1
    fk_send default "LOAD /etc/fkgpiod.conf"
}
on_signal() {
    [ -n "$APP" ] && kill "$APP" 2> /dev/null
}
trap on_signal TERM INT HUP

[ -f "$MAP" ] && fk_send load "LOAD $MAP" "$MAP"

./choochootracker > "$DATA/stderr.txt" 2>&1 &
APP=$!
wait "$APP"
RC=$?
# A trapped signal ends the first wait early; wait again for the real exit.
if kill -0 "$APP" 2> /dev/null; then wait "$APP"; RC=$?; fi
echo "launcher: app exited rc=$RC" >> "$LOG" 2>/dev/null
restore_map
exit "$RC"
