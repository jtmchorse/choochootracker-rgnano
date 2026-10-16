#!/bin/bash
# Builds the bundled SDL2 2.30.12 for the RG Nano: musl, ALSA, dummy+evdev video,
# no haptic/sensor, with the kernel's 16-byte input_event layout patched in (the
# SDK's musl uses 64-bit time_t, so its struct input_event is 24 bytes).
# Run inside the rgnano-build container. Output prefix: $WORK/stage-r3 (the
# default RGNANO_SDL2_ROOT of Makefile.rgnano). Idempotent: skips if built.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/root/work}
SDK=${RGNANO_SDK:-/opt/funkey-sdk/FunKey-sdk-2.3.0}
if [ -f "$WORK/stage-r3/lib/libSDL2-2.0.so.0" ]; then
  echo "SDL2 already built in $WORK/stage-r3"
  exit 0
fi
# shellcheck disable=SC1091
source "$SDK/environment-setup" >/dev/null
SYSROOT=$SDK/arm-funkey-linux-musleabihf/sysroot
mkdir -p "$WORK" && cd "$WORK"
[ -f SDL2-2.30.12.tar.gz ] || curl -sL -o SDL2-2.30.12.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-2.30.12.tar.gz
if [ ! -d SDL2-2.30.12-r3 ]; then
  mkdir x-r3 && tar xzf SDL2-2.30.12.tar.gz -C x-r3 && mv x-r3/SDL2-2.30.12 SDL2-2.30.12-r3 && rmdir x-r3
  bash "$HERE/sdl-evdev-16byte.sh" "$WORK/SDL2-2.30.12-r3"
fi
mkdir -p sdl-build-r3 && cd sdl-build-r3
export PKG_CONFIG_SYSROOT_DIR=$SYSROOT PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig
../SDL2-2.30.12-r3/configure --host=arm-funkey-linux-musleabihf --prefix="$WORK/stage-r3" \
  CFLAGS="-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard" \
  --disable-static --enable-shared \
  --enable-audio --enable-alsa --disable-alsa-shared --disable-pulseaudio --disable-jack --disable-pipewire --disable-sndio --disable-esd --disable-arts --disable-nas --disable-oss --disable-diskaudio \
  --enable-video-dummy --enable-input-events --disable-video-x11 --disable-video-wayland --disable-video-kmsdrm --disable-video-opengl --disable-video-opengles --disable-video-opengles1 --disable-video-opengles2 --disable-video-vulkan --disable-video-offscreen --disable-video-rpi --disable-video-vivante \
  --enable-joystick --disable-haptic --disable-hidapi --disable-sensor --disable-libudev --disable-dbus --disable-ime --disable-ibus --disable-fcitx --disable-libdecor \
  --enable-render --enable-threads --enable-timers --enable-cpuinfo --enable-atomic --enable-power --enable-filesystem --enable-loadso
make -j4
make install
