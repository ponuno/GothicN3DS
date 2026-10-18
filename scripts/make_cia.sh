#!/usr/bin/env bash
# Builds gothic3ds.cia (a real application for the HOME menu, New 3DS) from gothic3ds.elf.
# Needs: a finished `make` (gothic3ds.elf), and makerom + bannertool in PATH
#   makerom:    https://github.com/3DSGuy/Project_CTR      (cd makerom && make)
#   bannertool: https://github.com/carstene1ns/3ds-bannertool (cmake, make)
# The game data (data.pak, music.pak, speech.pak, dspfirm.cdc) stays on the SD card in /3ds/gothic3ds/ -
# the CIA contains none of it, so it may be shared like the .3dsx.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CIA="$ROOT/platform/3ds/cia"
OUT="${1:-$ROOT/gothic3ds.cia}"
ELF="$ROOT/gothic3ds.elf"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
[ -f "$ELF" ] || { echo "no $ELF - run make first"; exit 1; }
command -v makerom >/dev/null || { echo "makerom not found"; exit 1; }
command -v bannertool >/dev/null || { echo "bannertool not found"; exit 1; }
bannertool makebanner -i "$CIA/banner.png" -a "$CIA/jingle.wav" -o "$TMP/banner.bnr"
bannertool makesmdh -s "Gothic 3DS" -l "Gothic 1 for the New 3DS" -p "gothic3ds" -i "$CIA/icon.png" -o "$TMP/icon.smdh"
makerom -f cia -o "$OUT" -elf "$ELF" -rsf "$CIA/gothic3ds.rsf" -icon "$TMP/icon.smdh" -banner "$TMP/banner.bnr" -exefslogo -target t
echo "done: $OUT"
