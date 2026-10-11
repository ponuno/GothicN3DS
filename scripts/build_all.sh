#!/usr/bin/env bash
# One command for the whole chain: PC build -> (bake data.pak || 3DS build) -> copy to the SD folder.
#
#   scripts/build_all.sh [options]
#
#   --force        bake even if nothing the baker depends on changed
#   --no-bake      skip the baking
#   --no-3ds       skip the console build (also skipped when DEVKITARM is not set)
#   --no-check     bake with --no-script-check (minutes faster)
#   --test         run ctest after the PC build
#   --no-copy      do not copy to SD_DIR
#   --synth        bake the built-in test scene instead of the game (no game needed)
#
# Settings: scripts/build.conf (see build.conf.example) or ~/.gothic3ds.conf, or environment variables.
# The bake is skipped when the baker's sources, its flags and the game folder name are the same as for
# the last successful bake (stamp in OUT_DIR/.bake.stamp).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

FORCE=0 BAKE=1 BUILD3DS=1 CHECK=1 TEST=0 COPY=1 SYNTH=0
for a in "$@"; do
  case $a in
    --force) FORCE=1 ;; --no-bake) BAKE=0 ;; --no-3ds) BUILD3DS=0 ;; --no-check) CHECK=0 ;;
    --test) TEST=1 ;; --no-copy) COPY=0 ;; --synth) SYNTH=1 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

for f in scripts/build.conf "$HOME/.gothic3ds.conf"; do [ -f "$f" ] && . "$f"; done
GAME_DIR=${GAME_DIR:-}; OUT_DIR=${OUT_DIR:-$HOME/out}; MUSIC_DIR=${MUSIC_DIR:-}
SD_DIR=${SD_DIR:-}; BAKE_EXTRA=${BAKE_EXTRA:-}
mkdir -p "$OUT_DIR"
LOGS=$OUT_DIR/logs; mkdir -p "$LOGS"
step() { printf '\n== %s\n' "$*"; }
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)

# 1. PC tools
step "PC build"
[ -f build/CMakeCache.txt ] || cmake -S . -B build "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
if [ $TEST = 1 ]; then step "ctest"; ctest --test-dir build --output-on-failure; fi

# 2. Bake and the console build run side by side.
PIDS=() NAMES=()
if [ $BAKE = 1 ]; then
  (
    if [ $SYNTH = 1 ]; then
      ARGS=(synth -o "$OUT_DIR/data.pak")
    else
      [ -n "$GAME_DIR" ] || { echo "GAME_DIR is not set (scripts/build.conf)"; exit 1; }
      ARGS=(world --game "$GAME_DIR" -o "$OUT_DIR/data.pak")
      [ -n "$MUSIC_DIR" ] && ARGS+=(--music-dir "$MUSIC_DIR")
      [ $CHECK = 0 ] && ARGS+=(--no-script-check)
      # shellcheck disable=SC2206
      ARGS+=($BAKE_EXTRA)
    fi
    STAMP=$( { printf '%s\n' "${ARGS[@]}"
               find tools core engine third_party -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' \) -print0 \
                 | sort -z | xargs -0 sha1sum; } | sha1sum | cut -d' ' -f1)
    if [ $FORCE = 0 ] && [ -f "$OUT_DIR/data.pak" ] && [ "$(cat "$OUT_DIR/.bake.stamp" 2>/dev/null)" = "$STAMP" ]; then
      echo "bake: nothing changed since the last bake, skipped (--force to redo)"
    else
      echo "bake: ${ARGS[*]}"
      ./build/g3ds-bake "${ARGS[@]}"
      echo "$STAMP" > "$OUT_DIR/.bake.stamp"
    fi
  ) > "$LOGS/bake.log" 2>&1 &
  PIDS+=($!); NAMES+=(bake)
fi
if [ $BUILD3DS = 1 ] && [ -n "${DEVKITARM:-}" ]; then
  ( make -j"$(nproc)" ) > "$LOGS/3ds.log" 2>&1 &
  PIDS+=($!); NAMES+=(3ds)
elif [ $BUILD3DS = 1 ]; then
  echo "(DEVKITARM is not set: the .3dsx is not built)"
fi

FAIL=0
step "waiting for: ${NAMES[*]:-nothing}   (logs: $LOGS)"
for i in "${!PIDS[@]}"; do
  if wait "${PIDS[$i]}"; then echo "  ${NAMES[$i]}: ok"; else echo "  ${NAMES[$i]}: FAILED, see $LOGS/${NAMES[$i]/3ds/3ds}.log"; FAIL=1; fi
done
[ -f "$LOGS/bake.log" ] && [ $BAKE = 1 ] && { echo "-- bake report (tail):"; tail -n 25 "$LOGS/bake.log"; }
[ $FAIL = 0 ] || { [ -f "$LOGS/3ds.log" ] && tail -n 30 "$LOGS/3ds.log"; exit 1; }

# 3. Copy to the SD folder
if [ $COPY = 1 ] && [ -n "$SD_DIR" ]; then
  step "copy to $SD_DIR"
  mkdir -p "$SD_DIR"
  for f in gothic3ds.3dsx gothic3ds.smdh; do [ -f "$ROOT/$f" ] && cp -v "$ROOT/$f" "$SD_DIR/"; done
  for f in data.pak music.pak speech.pak; do [ -f "$OUT_DIR/$f" ] && cp -v "$OUT_DIR/$f" "$SD_DIR/"; done
fi
step "done"
