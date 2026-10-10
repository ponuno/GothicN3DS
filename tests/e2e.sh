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
	[ "$(num groundedFrames)" -gt 75 ] || fail "the walking player was not on the ground: $(num groundedFrames) of 150 frames"
	# Third person: the character is on the screen in (nearly) every frame, mostly running, sometimes in the air.
	[ "$(num heroFrames)" -gt 140 ] || fail "the character was drawn in only $(num heroFrames) of 150 frames"
	[ "$(num heroPixels)" -gt 300 ] || fail "the character covers only $(num heroPixels) pixels"
	[ "$(num heroRunFrames)" -gt 60 ] || fail "the character ran in only $(num heroRunFrames) frames"
	[ "$(num heroAirFrames)" -gt 5 ] || fail "the character never jumped"
	# The world is inhabited: the script's people stand where their routines put them.
	[ "$(num npcsTotal)" = "9" ] || fail "the scripts made $(num npcsTotal) characters, not the hero and eight more"
	[ "$(num npcsHere)" = "8" ] || fail "$(num npcsHere) characters have a place in the world, not 8"
	[ "$(num npcsShownMax)" -gt 2 ] || fail "only $(num npcsShownMax) inhabitants were ever on the screen"
	[ "$(num npcFrames)" -gt 100 ] || fail "inhabitants were drawn in only $(num npcFrames) of 150 frames"
	[ "$(num scriptErrors)" = "0" ] || fail "the scripts complained"
	[ "$(num npcStateCalls)" -gt 20 ] || fail "the inhabitants' states were not run: $(num npcStateCalls) calls"
	# They live their day: when the evening comes, the guard leaves his post
	# for the fire - along the paths, on his own feet.
	"$host" --pak "$out/data.pak" --still --cam 250,330,3300,188,-8 --time 19.9 --to-time 20.4 --frames 900 --shots 0 \
		--who --stats "$out/stats.json" > "$out/evening.txt"
	grep -q "^life              on: " "$out/evening.txt" || fail "no report of the inhabitants' life"
	grep -q "Thorus .* at " "$out/evening.txt" || fail "--who does not list the inhabitants"
	[ "$(num npcWalkingMax)" -gt 0 ] || fail "nobody walked when the evening came"
	[ "$(num npcPaths)" -gt 0 ] || fail "no way was found through the net"
	[ "$(num npcSnapped)" = "0" ] || fail "$(num npcSnapped) inhabitants could not walk to where they were going"
	[ "$(num npcClips)" -gt 8 ] || fail "only $(num npcClips) animations were read"
	[ "$(num scriptErrors)" = "0" ] || fail "the scripts complained in the evening"
	# By day the smith stands at his anvil; at night there is somebody in a bed.
	"$host" --pak "$out/data.pak" --still --cam -300,330,900,200,-10 --time 9 --frames 400 --shots 0 --who --stats "$out/stats.json" \
		> "$out/day.txt"
	grep -q "Digger  *BSANVIL " "$out/day.txt" || fail "the smith is not at his anvil"
	[ "$(num mobsUsed)" -gt 0 ] || fail "nothing was used by day"
	# The slope probe: the character stands, runs and jumps at a place, and says how it went.
	"$host" --pak "$out/data.pak" --slope-probe 0,400,4940 > "$out/slope.txt" || fail "the slope probe did not run"
	grep -q "^slope probe: 0 of 9 runs stuck" "$out/slope.txt" || fail "the slope probe found the character stuck on open ground"
	# ... at the console's 60 fps, from every metre around (a run may go through the portal: it comes back).
	"$host" --pak "$out/data.pak" --slope-probe 0,400,4940 --probe-fps 60 --probe-grid 1 > "$out/slope60.txt" || fail "the slope probe did not run at 60 fps"
	grep -q "^slope probe: 0 of 81 runs stuck" "$out/slope60.txt" || fail "the slope probe at 60 fps found the character stuck on open ground"
	"$host" --pak "$out/data.pak" --still --cam 600,330,650,62,-14 --time 23.6 --frames 700 --shots 0 --who --stats "$out/stats.json" \
		> "$out/night.txt"
	grep -q " BEDHIGH " "$out/night.txt" || fail "nobody went to bed"
	[ "$(num mobsInUse)" -gt 0 ] || fail "no bed is in use at night"
	# On request they only stand, as they used to.
	"$host" --pak "$out/data.pak" --still --time 19.9 --to-time 20.4 --frames 200 --shots 0 --standing --stats "$out/stats.json" \
		| grep -q "^life              off" || fail "--standing did not switch the inhabitants' life off"
	[ "$(num npcStateCalls)" = "0" ] || fail "states were run with --standing"
	[ "$(num npcWalkingMax)" = "0" ] || fail "somebody walked with --standing"
	# First person on request: nothing of the character is drawn.
	"$host" --pak "$out/data.pak" --walk --first-person --frames 30 --shots 1 --out "$out/eyes" --stats "$out/stats.json"
	[ "$(num heroFrames)" = "0" ] || fail "the character was drawn in first person"
	# The second world: a cave with a pool the scripted walk ends up swimming in.
	# (Wading is a walk at the walk animation's pace: it takes a while to reach deep water.)
	"$host" --pak "$out/data.pak" --world cave --walk --frames 220 --shots 1 --out "$out/cave" --stats "$out/stats.json"
	[ "$(num swimFrames)" -gt 30 ] || fail "nobody swam in the cave pool: $(num swimFrames) frames"
	[ "$(num npcsHere)" = "3" ] || fail "the cave has $(num npcsHere) inhabitants, not 3"
	[ "$(num npcsShownMax)" -gt 0 ] || fail "nobody was seen in the cave"
	[ "$(num heroSwimFrames)" -gt 30 ] || fail "the character did not swim: $(num heroSwimFrames) frames"
	[ "$(num blendedDrawsMax)" -gt 0 ] || fail "the water was not drawn in the blended pass"
	# The same flight with chunks streamed in the background, as on the console.
	for stream in inline thread; do
		"$host" --pak "$out/data.pak" --frames 150 --shots 1 --stream "$stream" --out "$out/$stream" --stats "$out/stats.json"
		[ "$(num loadFailures)" = "0" ] || fail "load failures with --stream $stream"
		[ "$(num chunksLoadedMax)" -gt 20 ] || fail "nothing was streamed in with --stream $stream"
	done
	# Without scripts the worlds are empty, as they used to be.
	"$bake" synth --cells 32 --no-scripts -o "$out/empty.pak" > "$out/empty.txt"
	grep -q "== scripts ==" "$out/empty.txt" && fail "--no-scripts baked scripts"
	"$host" --pak "$out/empty.pak" --walk --frames 10 --shots 0 --stats "$out/stats.json" | grep -q "scripts           none" \
		|| fail "an archive without scripts was not reported as such"
	[ "$(num npcsTotal)" = "0" ] || fail "inhabitants without scripts"
	"$host" --pak "$out/data.pak" --frames 150 --shots 2 --time 19.5 --out "$out/frame" --stats "$out/stats.json"
	;;
zen)
	# The loose-mesh path and the Gothic-install path must agree.
	"$bake" mesh --msh "$fixture/loose/fixture.msh" --textures "$fixture/loose" --chunk 2000 --max-tex 64 -o "$out/mesh.pak"
	# Names one world; the baker follows its level-change trigger to the other.
	# The fixture's character stands in for the game's (whose files it does not have).
	"$bake" world --game "$fixture/game" --zen FIXTURE.ZEN --chunk 2000 --max-tex 64 \
		--hero-script FIX_HERO.MDS --hero-body FIX_HERO_BODY --hero-head "" -o "$out/data.pak"
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

if [ "$mode" = synth ]; then
	grep -q "scripts: the built-in script: .* symbols" "$out/info.txt" || fail "no scripts in the report"
	grep -q "STARTUP_WORLD puts 8 characters into it (4 with a daily routine, 4 without), 1 items" "$out/info.txt" \
		|| fail "the start-up did not populate the world"
	grep -q "STARTUP_CAVE puts 3 characters into it" "$out/info.txt" || fail "the start-up did not populate the cave"
	grep -q "inhabitants: 12 characters with 6 different looks; models for 6 of them (11 characters): 3 skeletons" "$out/info.txt" \
		|| fail "the inhabitants' models were not baked"
	sed -n '/== scripts ==/,$p' "$out/info.txt" | grep -q "WARNING" && fail "the scripts' part of the bake has warnings"
	grep -q "places: 13 named" "$out/info.txt" || fail "the world's places were not baked"
	grep -q "way net: 13 points, 15 connections" "$out/info.txt" || fail "the way net was not baked"
	grep -q "life: the scripts have 11 states; the people of these worlds are put into 6: " "$out/info.txt" || fail "the states were not looked through"
	grep -q "life: the states call 39 engine functions, 39 of which the engine carries out" "$out/info.txt" || fail "the states' needs were not reported"
	grep -q "things to use: 8 of 6 kinds, with 10 places to stand at" "$out/info.txt" || fail "the things to use were not baked"
	grep -q "the scripts name 4 of them: BEDHIGH x2, BEDLOW x1, BENCH x1, BSANVIL x1; not named: CHESTBIG" "$out/info.txt" || fail "the things the scripts use were not reported"
	grep -q "  BENCH: T_BENCH_STAND_2_S0 0.40s ->S_BENCH_S0 root +0 +0 -36 (carries)" "$out/info.txt" || fail "what using the things looks like was not reported"
	grep -q "clusters: the chunks are cut into .* boxes of 10 m" "$out/info.txt" || fail "the chunks were not cut into clusters"
	grep -q "fighting 105 more; baked per skeleton: humans 61" "$out/info.txt" || fail "the fight animations were not baked"
	grep -q "S_1HATTACK 1.88s 3 blows" "$out/info.txt" || fail "the blows of an attack were not baked"
fi

if [ "$mode" = zen ]; then
	# The fixture's scripts: read from GOTHIC.DAT, checked against the reference
	# interpreter, run for both worlds; the people they name baked from the game's model files.
	grep -q "scripts: GOTHIC.DAT: 134 symbols" "$out/info.txt" || fail "the game's scripts were not read"
	grep -q "things to use: 6 of 2 kinds, with 2 places to stand at" "$out/info.txt" || fail "the fixture's bed was not baked"
	grep -q "checked against the reference interpreter: 6 runs (start-up, dialogues, items), 6 the same .* 0 different" "$out/info.txt" \
		|| fail "the scripts were not checked against the reference interpreter"
	grep -q "variables at the end the same" "$out/info.txt" || fail "the interpreters disagree about the variables"
	grep -q "STARTUP_FIXTURE puts 3 characters into it (1 with a daily routine, 2 without), 1 items" "$out/info.txt" \
		|| fail "the start-up did not populate the world"
	grep -q "5 characters with 4 different looks; models for 3 of them" "$out/info.txt" || fail "the inhabitants' models were not baked"
	grep -q "NOSUCH.MSB/.MDS not found" "$out/info.txt" || fail "a look without files was not reported"
	grep -q '"npcsTotal":4' "$out/stats.json" || fail "the scripts' characters did not reach the runtime"
	grep -q '"npcsHere":3' "$out/stats.json" || fail "the characters have no place in the world"
	# ... and they live: the one with a routine is in its state, which sends
	# it to its place and has it play an animation of the game's.
	grep -q "way net: 3 points, 1 connections" "$out/info.txt" || fail "the way net of the second world was not baked"
	grep -q "life: the scripts have 1 states" "$out/info.txt" || fail "the states were not found"
	grep -q '"npcStateCalls":[1-9]' "$out/stats.json" || fail "the state of the fixture's guard was not run"
	grep -q '"npcClips":[2-9]' "$out/stats.json" || fail "the animation the state asks for was not read"

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
	grep -q "untextured: 1 materials drawn in their own colour" "$out/info.txt" || fail "the material without a texture did not get its colour"
	grep -q "1 cut-out" "$out/info.txt" || fail "cut-out texture was not detected"
	grep -q "2 blended (1 water), 1 animated, 1 scrolling" "$out/info.txt" || fail "water or decal material lost its properties"
	# Both worlds are in the archive and their portals found each other.
	grep -q "start world: fixture" "$out/info.txt" || fail "no start world recorded"
	grep -q "fixture2 at ARRIVAL" "$out/info.txt" || fail "portal to the second world was not resolved"
	grep -q "fixture at START" "$out/info.txt" || fail "portal back was not resolved"
	# The character came along, from ZenGin's own model, mesh and animation files.
	grep -q "== model 'hero' ==" "$out/info.txt" || fail "no character in the archive"
	grep -q "skeleton: 5 nodes; mesh: 4 triangles" "$out/info.txt" || fail "the character's mesh was not baked"
	grep -q "rotations read conjugated" "$out/info.txt" || fail "the animations' rotation convention was not reported"
	grep -q "clips: 6 " "$out/info.txt" || fail "the character's clips were not baked"
	grep -q "blends: 1: T_RUNL_2_RUN 0.30s" "$out/info.txt" || fail "the model script's blends were not baked"
	grep -q "camera: from the game's CAMERA.DAT: CAMMODNORMAL range 3.00 m" "$out/info.txt" || fail "the game's camera scripts were not read"
	grep -q "S_WALKL 0.16s loop 2503cm/s" "$out/info.txt" || fail "the walk clip lost its speed"
	"$host" --pak "$out/data.pak" --walk --frames 30 --shots 1 --out "$out/hero" --stats "$out/stats3.json"
	grep -q '"heroFrames":[1-9]' "$out/stats3.json" || fail "the character was not drawn"
	"$host" --pak "$out/data.pak" --walk --frames 5 --shots 0 | grep -q "camera set-up: the game's" || fail "the game's camera set-up did not reach the runtime"
	# The game's own character is asked for by default; a game without it bakes none and says so.
	"$bake" world --game "$fixture/game" --zen FIXTURE.ZEN --no-follow -o "$out/nohero.pak" > "$out/nohero.txt"
	grep -q "HUMANS.MSB/.MDS not found, no character baked" "$out/nohero.txt" || fail "the missing character was not reported"
	"$host" --pak "$out/nohero.pak" --walk --frames 5 --shots 0 --stats "$out/stats4.json"
	grep -q '"heroFrames":0' "$out/stats4.json" || fail "a character appeared from nowhere"
	"$host" --pak "$out/data.pak" --world fixture2 --still --frames 3 --shots 1 --out "$out/second" --stats "$out/stats2.json"
	# A single world when asked not to follow.
	"$bake" world --game "$fixture/game" --zen FIXTURE.ZEN --no-follow -o "$out/single.pak" | grep -q "1 worlds" \
		|| fail "--no-follow still baked the second world"
fi
echo "e2e $mode OK"
