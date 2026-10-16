#!/bin/bash
# Patch a SDL2 2.30.12 source tree so the Linux evdev and joystick code reads the KERNEL's 16-byte input_event
# (32-bit ARM kernel) instead of musl-1.2's 24-byte struct input_event (64-bit time_t). Usage: sdl-evdev-16byte.sh <srcdir>
set -e
KEV=$(mktemp)
S="$1"
cat > "$KEV" <<'H'
/* rgnano: kernel struct input_event on 32-bit ARM is 16 bytes; musl 1.2 headers say 24. Use the kernel layout. */
#include <stdint.h>
struct rg_kernel_input_event { int32_t tv_sec, tv_usec; uint16_t type, code; int32_t value; };
#define input_event rg_kernel_input_event
H
for f in src/core/linux/SDL_evdev.c src/joystick/linux/SDL_sysjoystick_c.h; do
  # insert after the last #include <linux/input.h> (or equivalent) line
  python3 - "$S/$f" "$KEV" <<'P'
import sys,re
p=sys.argv[1]; t=open(p).read()
h=open(sys.argv[2]).read()
m=list(re.finditer(r'#include <linux/input\.h>\n',t))
assert m, "no linux/input.h include in "+p
i=m[-1].end()
t=t[:i]+h+t[i:]
open(p,'w').write(t)
P
done
grep -n "rg_kernel_input_event" "$S/src/core/linux/SDL_evdev.c" "$S/src/joystick/linux/SDL_sysjoystick_c.h" "$S/src/joystick/linux/SDL_sysjoystick.c"
