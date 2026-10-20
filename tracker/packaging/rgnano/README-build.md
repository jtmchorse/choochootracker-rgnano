# RG Nano (DrUm78 FunKey-OS) build

Target: Anbernic RG Nano, 240x240 fbtft panel, one Cortex-A7, 64 MB, musl.
Output: `releases/choochootracker-rgnano.opk` (GMenu2X OPK, squashfs).

## One command (PVE host)

    tracker/packaging/rgnano/build-opk.sh

It streams `chipnomad_lib/` and `tracker/` into the `rgnano-build` container
(CT 113), builds there with the FunKey SDK (`/opt/funkey-sdk/FunKey-sdk-2.3.0`,
GCC 10, musl, `-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`, static
libstdc++/libgcc), and copies the OPK back. The first run also builds the
patched SDL2 (`build-sdl2.sh`, a few minutes). Knobs: `CT=113`, `JOBS=4`,
`CLEAN=1` (fresh tree), `RGNANO_LTO=1` (passed through make, off by default).

Before building, `build-opk.sh` runs `make -f Makefile.test RGNANO=1 opk-gate`
and aborts if any of these fail: `voice_alloc_check` (lazy vs eager renders,
including track-0-family-differs), `stretch_lifetime`, `beads_lifetime`, and the
doctest cases `Project voice pools*` (engine lifetime) and `export: limiter*`
(limiter off is bit-identical). The 8 known host failures in the full suite are
not part of the gate. There is no skip switch.

Inside the container the equivalent is
`cd tracker && make -f Makefile.rgnano -j4 RGNano-deploy`
(`make RGNano-deploy` through the top-level `makefile` works too).

## Files here

| File | Purpose |
|---|---|
| `build-opk.sh` | host-side one-command build |
| `build-sdl2.sh`, `sdl-evdev-16byte.sh` | SDL2 2.30.12 for the device: ALSA + dummy/evdev video, kernel 16-byte `input_event` patch (the SDK's musl uses a 24-byte struct, the kernel emits 16) |
| `choochootracker.sh` | launcher: cd, `LD_LIBRARY_PATH=libs`, `SDL_VIDEODRIVER=evdev`, finds the `ample` uinput button node by Name, execs |
| `choochootracker.funkey-s.desktop`, `choochootracker.png` | GMenu2X entry and 32x32 icon |
| `qemu-run.sh` | boots the OPK contents under `qemu-arm-static` (run in the container) |

## libm override

The SDK sysroot `libc.so` has miscompiled `exp`, `expf`, `pow`, `powf` (bad
argument reduction: `expf(-0.0004)` returns exactly 1.0, `expf(-0.1)` returns
0.897). `platforms/rgnano_libm/` vendors musl 1.2.1's own sources for them
(plus `exp2`, `exp2f`, data tables, `__math_*` helpers; MIT) and
`Makefile.rgnano` compiles them with `-O2 -fno-fast-math -ffp-contract=off`
into the executable, so they win over `libc.so` at dynamic link time. The
startup `libm sys: ... | override: ...` log line calls the system versions via
`dlsym(RTLD_NEXT)` to keep measuring the real library.

## Runtime layout

- OPK mount (read-only): binary, `libs/libSDL2-2.0.so.0`, bundled assets, `title/`, `licenses/`.
- `/mnt/ChooChooTracker/` (SD card, `RGNANO_DATA_DIR` overrides): settings.txt,
  autosave.cct, `rgnano-log.txt` (+ `.prev`), `launcher.txt`, `stderr.txt`, and a
  seeded copy of projects, samples, instruments, wavetables, waveforms,
  pitch-tables, themes and fonts. Seeding copies only missing files, once per
  app version/build (marker `.seeded`). All app paths (projects, samples, exports,
  presets) point there.
- `keymap-preset.txt` in that directory (first launch only): `shift-r1` moves Shift
  from Select (Q) to R (N). `lsdj` is the default.

## Display

The UI renders into a software-renderer composition texture of any logical size
(default 480x320, `RGNANO_CANVAS=WxH` overrides). Each dirty frame is read back
and area-downscaled (exact fractional box filter) to 240x240 RGB565, then copied
to the mmap'd `/dev/fb0`. Debug env vars:

- `RGNANO_FB_DUMP=<file>`: write the 240x240 RGB565 frame (115200 bytes, little
  endian) to a file instead of `/dev/fb0` (at most every 100 ms and at exit).
- `RGNANO_FB_PPM=<file>` (+ `RGNANO_FB_PPM_SECS=N`, default 5): also write a P6 PPM.
- `RGNANO_FB_DEV=<path>`: framebuffer node override.
- Present timings (read, scale, blit) are logged every 300 frames.

## Audio

`rgnano_platform.cpp` opens audio first thing in `main()`, before video/evdev,
in a worker thread with a 3 s watchdog. On timeout the thread is abandoned and
the speaker (`V3s Audio Codec`) is opened by name; if that also fails, a
software clock drives the engine so playback and the UI still run. Everything is
logged to `rgnano-log.txt`. 32000 Hz S16 stereo, 512 frames, flush-to-zero set
on the audio thread. `RGNANO_AUDIO_DEVICE=<SDL device name>` forces a device;
`RGNANO_AUDIO_TEST_HANG=1` emulates the round-3 hang for the watchdog test.

The master bus ends in a peak limiter (`chipnomad_lib/synth/master_limiter.h`):
stereo-linked, -0.3 dBFS ceiling, 1.5 ms look-ahead, 80 ms release, about 0.4% of
the A7 at 32 kHz (estimated). Settings > Mixer > "Master limiter" (settings.txt
`masterLimiter`, default ON here, OFF on every other platform). Mix volume
defaults to 50% (-6 dB) so a summed 8-track mix leaves the limiter near idle;
raise it on the same screen. The `astat` log lines carry `limiter GRmax=.. GRavg=..
engaged=..%`. The Mixer CLIP! flag means the bus was over full scale BEFORE the
limiter, i.e. it would have clipped.

## Controls (SDL keys from the `ample` evdev device)

Up/Down/Left/Right = U/D/L/R, A = edit, B = option, Start = play,
Select (Q) = shift, L = M, R = N, Fn = K. Fn+L = V and Fn+R = O (motion
record/erase slots). Fn (K) alone is swallowed; Fn + X is the quit chord on
platforms that deliver it, and Settings > Quit always works. The Controls >
Key mapping screen still remaps everything.

## Checks

    # in the container, after build-opk.sh
    /root/work/qemu-run.sh 20            # boots, runs 20 s, dumps the frame
    readelf -d tracker/build/rgnano/choochootracker | grep NEEDED   # SDL2 + libc only
    sh -n tracker/packaging/rgnano/choochootracker.sh

WAV export (single mixed file and bounce) runs the same mix trim and master limiter as live playback when Settings > master limiter is on, so exports match what the device plays; stem export stays unlimited.
