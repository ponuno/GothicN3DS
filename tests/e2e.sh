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
	# Walking with collision: the player must spend most of the time on the ground.
	"$host" --pak "$out/data.pak" --walk --frames 150 --shots 1 --out "$out/walk" --stats "$out/stats.json"
	[ "$(num groundedFrames)" -gt 100 ] || fail "the walking player was not on the ground: $(num groundedFrames) of 150 frames"
	# The second world: a cave with a pool the scripted walk ends up swimming in.
	"$host" --pak "$out/data.pak" --world cave --walk --frames 150 --shots 1 --out "$out/cave" --stats "$out/stats.json"
	[ "$(num swimFrames)" -gt 30 ] || fail "nobody swam in the cave pool: $(num swimFrames) frames"
	[ "$(num blendedDrawsMax)" -gt 0 ] || fail "the water was not drawn in the blended pass"
	# The same flight with chunks streamed in the background, as on the console.
	for stream in inline thread; do
		"$host" --pak "$out/data.pak" --frames 150 --shots 1 --stream "$stream" --out "$out/$stream" --stats "$out/stats.json"
		[ "$(num loadFailures)" = "0" ] || fail "load failures with --stream $stream"
		[ "$(num chunksLoadedMax)" -gt 20 ] || fail "nothing was streamed in with --stream $stream"
	done
	"$host" --pak "$out/data.pak" --frames 150 --shots 2 --time 19.5 --out "$out/frame" --stats "$out/stats.json"
	;;
zen)
	# The loose-mesh path and the Gothic-install path must agree.
	"$bake" mesh --msh "$fixture/loose/fixture.msh" --textures "$fixture/loose" --chunk 2000 --max-tex 64 -o "$out/mesh.pak"
	# Names one world; the baker follows its level-change trigger to the other.
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
grep -q "seams: .* T-junctions closed" "$out/info.txt" || fail "no seam repair in the report"
grep -q "position step: " "$out/info.txt" || fail "no position step in the report"
grep -q "inside out" "$out/info.txt" && fail "winding check complained about a correct world"

if [ "$mode" = zen ]; then
	# The check must catch a deliberately inverted bake.
	"$bake" world --game "$fixture/game" --zen FIXTURE.ZEN --flip -o "$out/flipped.pak" | grep -q "inside out" \
		|| fail "winding check missed an inverted world"
	# 514 world triangles plus the objects: four boxes, two models, a decal.
	grep -q "590 triangles in, 590 out" "$out/info.txt" || fail "world bake lost triangles"
	grep -q "objects: 7 placed" "$out/info.txt" || fail "objects were not baked"
	grep -q "ladders: 1" "$out/info.txt" || fail "the ladder was not baked"
	grep -q "indoor world: no sky, background colour 30 20 10" "$out/info.txt" || fail "the second world is not indoor"
	grep -q "2 models in rest pose" "$out/info.txt" || fail "models were not baked"
	grep -q "1 missing" "$out/info.txt" || fail "missing texture was not reported"
	grep -q "1 cut-out" "$out/info.txt" || fail "cut-out texture was not detected"
	grep -q "2 blended (1 water), 1 animated, 1 scrolling" "$out/info.txt" || fail "water or decal material lost its properties"
	# Both worlds are in the archive and their portals found each other.
	grep -q "start world: fixture" "$out/info.txt" || fail "no start world recorded"
	grep -q "fixture2 at ARRIVAL" "$out/info.txt" || fail "portal to the second world was not resolved"
	grep -q "fixture at START" "$out/info.txt" || fail "portal back was not resolved"
	"$host" --pak "$out/data.pak" --world fixture2 --still --frames 3 --shots 1 --out "$out/second" --stats "$out/stats2.json"
	# A single world when asked not to follow.
	"$bake" world --game "$fixture/game" --zen FIXTURE.ZEN --no-follow -o "$out/single.pak" | grep -q "1 worlds" \
		|| fail "--no-follow still baked the second world"
fi
echo "e2e $mode OK"
