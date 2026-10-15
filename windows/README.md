# Baking the game data on Windows (no WSL, no installs)

> **The game's data is not distributed.** You need your own legal copy of Gothic. The files you bake (`data.pak`, `music.pak`, `speech.pak`) contain the game's content: keep them for yourself and never publish them.

## What you need

- Windows 10 or 11 (64-bit).
- Your own copy of **Gothic** (Steam, GOG or disc), installed.
- From the project's **Releases** page, three files:
  - `Gothic3DS-Baker.exe` - the window you use
  - `g3ds-bake.exe` - the converter it calls
  - `dmrender.exe` - renders the game's music to sound files
- `gothic3ds.3dsx` from the same release (the game itself, it contains no game data).

## Steps

1. Create a new folder anywhere (for example `C:\Gothic3DS`) and put **all three `.exe` files in the same folder**.
2. Double-click **`Gothic3DS-Baker.exe`**.
   - Windows SmartScreen may say "Windows protected your PC" because the program is new and unsigned. Click **More info**, then **Run anyway**.
3. Press **Browse...** and select your Gothic folder: the one that contains the `Data` and `_work` folders (Steam: `...\steamapps\common\Gothic`). It is filled in for you if the game is in a usual place.
4. Leave **Also bake the music** ticked (music adds 10-30 minutes; untick it to skip music).
5. Press **Bake** and wait. The log shows what is happening; the window may look idle for minutes during the long steps. Do not close it.
6. When it says **Done**, these files are in the **same folder as the `.exe` files**:
   - `data.pak` - worlds, characters, sounds
   - `music.pak` - music (only if the music box was ticked)
   - `speech.pak` - voices
   - `bake.log` - the full log (send it if something goes wrong)
   - `bake_debug.txt` - only with the "Debug report" box ticked: what the game's scripts did while the worlds were populated, which engine functions and Ikarus/LeGo/Union code are missing (send it with `bake.log` for a modded game)

The window has two more boxes: **Bake the voices** (untick to skip `speech.pak`: faster, smaller; the game then shows subtitles only) and **Debug report**.

Two more: **Original water texture** (the game's own water instead of the baker's soft caustics) and **Grass report for testing** (writes `grass_<world>.txt` and `.png` pictures next to the program: where new grass could grow and what it would cost; `data.pak` is the same with or without it).

## Put it on the 3DS

Copy to the SD card:

```
SD:/3ds/gothic3ds/gothic3ds.3dsx
SD:/3ds/gothic3ds/data.pak
SD:/3ds/gothic3ds/music.pak
SD:/3ds/gothic3ds/speech.pak
SD:/3ds/dspfirm.cdc          <- once, dumped from your console with DSP1 (needed for any sound)
```

Start the game from the Homebrew Launcher. More detail (including the DSP firmware dump): [README.md](../README.md), step 8.

## Troubleshooting

| Problem | What to do |
|---|---|
| "This folder has no Data subfolder" | Select the folder where Gothic is installed, the one that contains `Data` and `_work`, not `Data` itself. |
| "g3ds-bake.exe is not next to this program" | All three `.exe` files must be in the same folder. |
| No `music.pak` | Music was unticked, or `dmrender.exe` is missing, or the music could not be rendered. Read the log; the rest still works, the game is just silent apart from effects. |
| The window seems frozen | It is working. Baking takes several minutes, music rendering up to 30 more. |
| Antivirus quarantines an `.exe` | False alarm for new unsigned programs. Allow it, or build the programs yourself (below). |
| Failed | Send `bake.log` (next to the `.exe` files) when asking for help. |

Gothic II and mods: run `g3ds-bake.exe` from a command prompt, for example `g3ds-bake.exe world --game "C:\Games\Gothic II" --mod Archolos -o data.pak --no-script-check` (see `g3ds-bake.exe --help`).

## Build the programs yourself

Run `build_baker.bat` in this folder (needs internet; installs the free MSYS2 compiler kit with `winget`; 10-15 minutes). It creates the three `.exe` files here and starts the window. The automatic build for releases is `.github/workflows/windows-baker.yml`.

The reference-interpreter script check is skipped on Windows (it is a developer's check, not needed to play).
