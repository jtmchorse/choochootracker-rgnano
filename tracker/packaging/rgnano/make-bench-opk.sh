#!/bin/bash
# Build rgnano-bench2.opk on the PVE host (bench via CT 113, like build-opk.sh).
#   make-bench-opk.sh EST.tsv OUT.opk
# EST.tsv is the x86 estimate table from: bench_device --host-est EST.tsv (built with
#   make -f Makefile.test bench-device BUILD_DIR=build/benchhost CFLAGS='-std=c++17 -O2 -w -DNDEBUG -DTEST -DTEST_PORTMASTER_INPUT -DRGNANO_BUILD').
# The bench links no SDL, so the bundled SDL2 is not needed. Each run uses a fresh
# directory in the container (DEST) so nothing has to be deleted first.
set -euo pipefail
EST=${1:?est table}; OUT=${2:?output opk}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
CT=${CT:-113}
DEST=${DEST:-/root/work/ccr-p5-$(date +%s)}
JOBS=${JOBS:-4}
SDK=/opt/funkey-sdk/FunKey-sdk-2.3.0
run() { pct exec "$CT" -- bash -lc "$1"; }
run "mkdir -p $DEST"
tar -C "$REPO" --exclude=tracker/build -cf - chipnomad_lib tracker | pct exec "$CT" -- tar -x -m -C "$DEST"
pct exec "$CT" -- tee $DEST/bench-est.tsv < "$EST" > /dev/null
ARM="-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
run "cd $DEST/tracker && make -f Makefile.test -j$JOBS bench-device BUILD_DIR=build/bench-arm \
  CXX=$SDK/bin/arm-funkey-linux-musleabihf-g++ \
  CFLAGS='-std=c++17 -O2 -w -DNDEBUG -DTEST -DTEST_PORTMASTER_INPUT -DRGNANO_BUILD $ARM' \
  LDFLAGS='-static-libstdc++ -static-libgcc -lpthread -lm'"
P=$DEST/pkg
run "mkdir -p $P/projects $P/samples/ST-01 $P/waveforms/AKWF $P/SR_wavetables/WaveEdit && T=$DEST/tracker/packaging/common && \
  cp \$T/projects/gm-midi-demo.cct $P/projects/ && cp \$T/samples/ST-01/strings6.wav $P/samples/ST-01/ && \
  cp \$T/waveforms/AKWF/AKWF_cello_0001.wav $P/waveforms/AKWF/ && cp \$T/SR_wavetables/WaveEdit/PPG_WA04.WAV $P/SR_wavetables/WaveEdit/ && \
  B=$DEST/tracker/packaging/rgnano/bench && cp \$B/rgnano-bench2.sh \$B/rgnano-bench2.funkey-s.desktop \$B/rgnano-bench2.png $P/ && \
  cp $DEST/bench-est.tsv $P/ && $SDK/bin/arm-funkey-linux-musleabihf-strip -o $P/bench_device $DEST/tracker/build/bench-arm/bench_device && \
  chmod 755 $P/rgnano-bench2.sh $P/bench_device && \
  mksquashfs $P $DEST/rgnano-bench2.opk -all-root -noappend -no-exports -no-xattrs > /dev/null && ls -l $DEST/rgnano-bench2.opk"
pct exec "$CT" -- cat $DEST/rgnano-bench2.opk > "$OUT"
ls -l "$OUT"; sha256sum "$OUT"
