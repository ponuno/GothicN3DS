# Gothic 3DS — Gothic 1 on the New Nintendo 3DS

> **Game data is not distributed.** You need your own legal copy of Gothic 1. The files you bake from it (`data.pak`, `music.pak`, `speech.pak`, `video.pak`, the music WAVs) contain the game's content: keep them for yourself and never publish them. Only the source code, the tools and the compiled `gothic3ds.3dsx` / `gothic3ds.cia` (they contain no game data) may be shared.

A port of **Gothic 1** to the **New Nintendo 3DS** on its own engine: C++17, a renderer for the PICA200 GPU and a Daedalus script VM. A PC tool bakes your original game files into compact archives; the console runs the world from them: scripts, dialogue, quests, combat, magic, townsfolk with daily routines, sound, music, voices and videos.

**Status:** Gothic 1 is playable on a real New 3DS and is being played through chapter by chapter; bugs are fixed as they turn up (see [`docs/HANDOFF.md`](docs/HANDOFF.md)). Old 3DS models are not supported. Gothic 2 and mods (Chronicles of Myrtana: Archolos) bake and start, but are not playable yet.

## What works

- **World:** the real Gothic 1 worlds streamed in 40 m chunks, draw distance up to 200 m, day and night, weather, water, portals and interiors, doors, levers, lifts, gates, scripted camera flights, Bink videos (re-encoded by the baker).
- **Hero:** skeletal animation, running, swimming, ladders, jumping and climbing, sneaking; two control schemes (console and classic PC); inventory, quick slots and the diary on the bottom screen; saves with 10 slots, quick save and autosave.
- **Combat and magic:** the game's own rules: target lock, combos, parry, bows and crossbows, runes and scrolls, transformations, summoning; townsfolk fight, shoot and cast too.
- **Townsfolk:** the original scripts: daily routines, perception (sight, hearing, theft, entering houses), dialogue, quests and chapters, teachers, trading.
- **Graphics:** a menu with presets (Speed 60 fps / Balanced / Quality 30 fps) and **Graphics+** for every option: ambient occlusion, swaying plants, new grass, water waves, sun and character shadows, fire light, far layer, anti-aliasing, 3D or wide 800×240 screen. Some options need data baked with extra switches (see the table in step 4).
- **Interface** in Russian, English, Polish and German (the game's texts come from your copy).

**Not there yet:** ZenGin `.CS` cutscenes, gossip between townsfolk, Ikarus/LeGo (needed by Archolos).

## What you need

- A **New Nintendo 3DS / New 2DS XL** with custom firmware (Luma3DS) and the Homebrew Launcher.
- **The DSP firmware** dumped once with the **DSP1** app to `sdmc:/3ds/dspfirm.cdc`. Without it the game runs but is completely silent.
- Gothic 1 installed on the PC (Steam or GOG).
- An SD card with ~3 GB free for the baked data.

## Option A: Windows, no compiling (recommended)

1. From the Releases page download `Gothic3DS-Baker.exe`, `g3ds-bake.exe` and `dmrender.exe` into one folder, plus the ready `gothic3ds.3dsx` or `gothic3ds.cia`.
2. Run `Gothic3DS-Baker.exe`, pick your Gothic folder, press **Bake**. ffmpeg (for the videos) is used if you have it, otherwise downloaded once by the baker. The `.pak` files appear next to it. Details: [`windows/README.md`](windows/README.md).
3. Copy everything to the SD card (step 6 below).

## Option B: build everything yourself (Linux or WSL)

All commands run in an Ubuntu terminal (on Windows: `wsl --install -d Ubuntu`). Keep the project in your Linux home folder (`~`), not under `/mnt/c`: it is many times slower there. In WSL, drive `C:` is `/mnt/c`; quote paths with spaces.

### 1. Tools and the project

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build git zip unzip python3 wget
cd ~ && unzip -q gothic3ds.zip && cd ~/gothic3ds
cmake -S . -B build -G Ninja
cmake --build build -j
```

This builds `build/g3ds-bake` (the baker) and `build/g3ds-host` (the same engine on the PC, for debugging). GCC 11+ and CMake 3.16+ are needed. Tests (optional): `ctest --test-dir build`.

### 2. Music (once)

Gothic's music is DirectMusic, which the 3DS cannot play, so `dmrender` renders every theme to a seamless loop:

```sh
cmake -S tools/dmrender -B build-dmrender && cmake --build build-dmrender
build-dmrender/dmrender "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic/_work/DATA/Music" ~/music_wavs
```

For games whose music is inside `.vdf`/`.mod` archives, first extract it: `./build/g3ds-bake extract --game "<game folder>" -o ~/music_src`, then render from `~/music_src`.

### 3. Bake the data

```sh
mkdir -p ~/out
./build/g3ds-bake world --game "/mnt/c/Program Files (x86)/Steam/steamapps/common/Gothic" \
    -o ~/out/data.pak --music-dir ~/music_wavs
```

You get `data.pak` (worlds, models, sounds), `music.pak`, `speech.pak` (voices, 1–2 GB at full quality) and `video.pak` (the videos are decoded with ffmpeg: one in `PATH` or beside the baker is used; on Windows, if there is none, the baker downloads it once by itself; on Linux install it with `sudo apt install ffmpeg`). The report is saved in `~/out/bake.log`; `./build/g3ds-bake info ~/out/data.pak` prints it again. **Bake again after every project update** if `docs/HANDOFF.md` says so for that version.

Instead of steps 1–3 and 5, `scripts/build_all.sh` does everything in one go; its settings are in `scripts/build.conf` (copy `scripts/build.conf.example`).

### 4. Optional baker switches

| Switch | What it adds (turned on in Graphics+) |
|---|---|
| `--ao low` / `--ao high` | Shading in corners (ambient occlusion) |
| `--wind` | Trees, bushes and grass sway |
| `--grass` | New grass on the meadows |
| `--water-grid` | Waves on the water |
| `--sun-shadow` | The sun's shadow from trees and houses |
| `--far-lod` | A simplified far layer beyond the draw distance |
| `--lod` | Simplified far chunks |
| `--vis` | Visibility table (hidden parts are not drawn) |
| `--voice-codec ima`, `--voice-rate 11025` | Smaller `speech.pak`; `--no-voices` skips voices |
| `--no-video`, `--video-fps 10` | Skip or shrink the videos |
| `--mod NAME` | Gothic 2 mods: bake the mod named by `System/NAME.ini` |

Without a switch the matching menu option simply does nothing.

### 5. The 3DS program (optional; a ready one is on the Releases page)

Install devkitPro (`sudo dkp-pacman -S 3ds-dev`, see [devkitpro.org](https://devkitpro.org/wiki/Getting_Started)), set `DEVKITPRO=/opt/devkitpro` and `DEVKITARM=/opt/devkitpro/devkitARM`, then:

```sh
make              # gothic3ds.3dsx (+ .smdh) for the Homebrew Launcher
make cia          # gothic3ds.cia for the HOME menu
```

`make cia` also needs `makerom` ([Project_CTR](https://github.com/3DSGuy/Project_CTR)) and `bannertool` ([3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool)) in `PATH`. The icon, banner and app settings are in `platform/3ds/cia/`.

### 6. The SD card

```
/3ds/dspfirm.cdc                <- the DSP firmware, directly in /3ds
/3ds/gothic3ds/gothic3ds.3dsx   <- or install gothic3ds.cia with FBI instead
/3ds/gothic3ds/data.pak
/3ds/gothic3ds/music.pak
/3ds/gothic3ds/speech.pak
/3ds/gothic3ds/video.pak
```

Start it from the Homebrew Launcher (or the HOME menu for the CIA). Settings, saves and reports (`sound.txt`, `hang.txt`) are kept in `/3ds/gothic3ds/`.

## Troubleshooting

| Problem | Fix |
|---|---|
| No sound at all | `/3ds/dspfirm.cdc` is missing: run DSP1 once. `sound.txt` then says `dsp: NOT open (ndspInit 0xD880A7FA)` |
| Sound works, nobody speaks / no music | `speech.pak` / `music.pak` is not next to `data.pak`, or its slider in the Sound menu is 0 |
| A Graphics+ option does nothing | `data.pak` was baked without its switch (step 4) |
| The game crashes or hangs | Send `/3ds/gothic3ds/hang.txt` (and the Luma crash screen's PC and LR) to the developer |
| `unable to find a build program corresponding to "Ninja"` | `sudo apt install -y ninja-build`, delete `build`, repeat step 1 |
| `Please set DEVKITARM in your environment` | Set the variables from step 5 (`source ~/.bashrc`) |
| `makerom not found` on `make cia` | Put `makerom` and `bannertool` in a folder on your `PATH` and open a new terminal |
| The baker cannot find `WORLD.ZEN` | `--game` must point to the folder with `Data` and `_work` |
| Everything is very slow in WSL | The project is under `/mnt/c`: move it to `~` |

## More

- [`docs/TECHNICAL.md`](docs/TECHNICAL.md): how the engine works, controls in detail.
- [`docs/HANDOFF.md`](docs/HANDOFF.md): every version's changes, what needs a re-bake, what is verified on the console.
- [`docs/ROADMAP.md`](docs/ROADMAP.md): plans.
- [`docs/FORMATS.md`](docs/FORMATS.md): the baked file formats.
- [`third_party/README.md`](third_party/README.md): third-party code and licences. The project's own licence is not chosen yet.
