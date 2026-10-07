#!/bin/sh
# End-to-end test through the command-line tools:
#   bake -> data.pak -> info -> runtime (software backend) -> screenshots + stats
# usage: e2e.sh synth|zen <g3ds-bake> <g3ds-host> <out dir> [zen fixture dir]
set -eu
mode=$1
bake=$2
host=$3
out=$4
fixture=${5:-}

rm -rf "$out"
mkdir -p "$out"

num() { sed -n "s/.*\"$1\":\([0-9.]*\).*/\1/p" "$out/stats.json"; }
fail() { echo "e2e FAIL: $*"; exit 1; }

case "$mode" in
synth)
	"$bake" synth --cells 96 -o "$out/data.pak"
	"$host" --pak "$out/data.pak" --frames 150 --shots 2 --out "$out/frame" --stats "$out/stats.json"
	;;
zen)
	# The loose-mesh path and the Gothic-install path must agree.
	"$bake" mesh --msh "$fixture/loose/fixture.msh" --textures "$fixture/loose" --chunk 2000 --max-tex 64 -o "$out/mesh.pak"
	"$bake" world --game "$fixture/game" --zen FIXTURE.ZEN --chunk 2000 --max-tex 64 -o "$out/data.pak"
	"$bake" info "$out/mesh.pak" | grep -q "514 triangles in, 514 out" || fail "mesh bake lost triangles"
	"$host" --pak "$out/data.pak" --still --frames 3 --shots 1 --out "$out/frame" --stats "$out/stats.json"
	;;
*)
	fail "unknown mode $mode"
	;;
esac

"$bake" info "$out/data.pak" > "$out/info.txt"
cat "$out/info.txt"
grep -q "world " "$out/info.txt" || fail "info did not print the bake report"

[ "$(num loadFailures)" = "0" ] || fail "load failures"
[ "$(num budgetRejects)" = "0" ] || fail "budget rejects"
[ "$(num trianglesMax)" -gt 100 ] || fail "nothing was drawn"
[ "$(num pixelsWritten)" -gt 20000 ] || fail "frame is empty"
# Mostly front faces: the winding convention survived the whole chain.
awk "BEGIN { exit !($(num backfaceShare) < 0.5) }" || fail "too many back faces: $(num backfaceShare)"
[ -s "$out/frame_00.png" ] || fail "no screenshot"

grep -q "winding check" "$out/info.txt" || fail "no winding check in the report"
grep -q "inside out" "$out/info.txt" && fail "winding check complained about a correct world"

if [ "$mode" = zen ]; then
	# The check must catch a deliberately inverted bake.
	"$bake" world --game "$fixture/game" --zen FIXTURE.ZEN --flip -o "$out/flipped.pak" | grep -q "inside out" \
		|| fail "winding check missed an inverted world"
	grep -q "514 triangles in, 514 out" "$out/info.txt" || fail "world bake lost triangles"
	grep -q "1 missing" "$out/info.txt" || fail "missing texture was not reported"
	grep -q "1 cut-out" "$out/info.txt" || fail "cut-out texture was not detected"
fi
echo "e2e $mode OK"
