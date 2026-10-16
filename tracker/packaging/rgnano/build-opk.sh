#!/bin/bash
# One-command RG Nano build, run on the PVE host:
#   tracker/packaging/rgnano/build-opk.sh
# Syncs this worktree into the rgnano-build container (CT 113), builds the app
# there (incremental; CLEAN=1 for a fresh tree) and copies the OPK back to
# releases/choochootracker-rgnano.opk. The container needs the FunKey SDK in
# /opt/funkey-sdk; the patched SDL2 is built on first use (build-sdl2.sh).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
CT=${CT:-113}
DEST=${DEST:-/root/work/ccr}
JOBS=${JOBS:-4}
run() { pct exec "$CT" -- bash -lc "$1"; }

if [ "${CLEAN:-0}" = 1 ]; then run "rm -rf $DEST"; fi
run "mkdir -p $DEST"
# The container cannot write host-created files, so stream a tar through it.
tar -C "$REPO" --exclude=tracker/build -cf - chipnomad_lib tracker \
  | pct exec "$CT" -- tar -x -m -C "$DEST"
run "bash $DEST/tracker/packaging/rgnano/build-sdl2.sh"
run "cd $DEST/tracker && make -f Makefile.rgnano -j$JOBS RGNano-deploy"
mkdir -p "$REPO/releases"
pct exec "$CT" -- cat "$DEST/releases/choochootracker-rgnano.opk" > "$REPO/releases/choochootracker-rgnano.opk"
ls -l "$REPO/releases/choochootracker-rgnano.opk"
