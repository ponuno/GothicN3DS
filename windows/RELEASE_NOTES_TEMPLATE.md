## Files in this release

| File | What it is |
|---|---|
| `gothic3ds.3dsx` | The game for the Nintendo 3DS (contains no game data) |
| `Gothic3DS-Baker.exe` | Windows: window that bakes your game files into `data.pak` / `music.pak` / `speech.pak` |
| `g3ds-bake.exe` | Windows: the converter used by the window (keep it in the same folder) |
| `dmrender.exe` | Windows: renders the game's music (keep it in the same folder) |

**Quick start (Windows):** put the three `.exe` files in one folder, run `Gothic3DS-Baker.exe`, pick your Gothic folder, press **Bake**. The `.pak` files appear next to the `.exe`. Copy them with `gothic3ds.3dsx` to `SD:/3ds/gothic3ds/`. Full guide: `windows/README.md`.

You need your own legal copy of Gothic. The baked files contain the game's content: never publish them.

SmartScreen/antivirus may warn about the unsigned `.exe` files: choose *More info -> Run anyway*.
