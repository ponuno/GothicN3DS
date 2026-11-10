#!/bin/sh
# Bakes the synthetic lake scene and renders two frames from the shore, for
# looking at the water (waves, glint, Fresnel). Host only: the PC backend has
# no reflection target, so use the console for the mirror pass.
#
#   scripts/water_scene.sh [OUT_DIR]
#
# Needs a configured build directory (default: build, override with BUILD=...).
set -eu
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build}
OUT=${1:-water-test}
mkdir -p "$OUT"

"$BUILD/g3ds-bake" synth -o "$OUT/synth_water.pak" \
	--water-grid 2 --water-clear --water-reflect 0.5 --wind --ao low
"$BUILD/g3ds-host" --pak "$OUT/synth_water.pak" \
	--frames 120 --shots 2 --waves 3 --water-fx 2 --still \
	--cam 6500,-50,-2400,180,-20 --time 13 --out "$OUT/lake"
printf 'frames: %s/lake_00.png, %s/lake_01.png\n' "$OUT" "$OUT"
