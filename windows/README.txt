Windows: baking data.pak without WSL
====================================
You need only your own copy of Gothic. No WSL, no Linux.

1. Take g3ds-bake.exe (from the release / CI artifact "g3ds-bake-windows") and put it next to bake.bat.
   No .exe? Run build_baker.bat once: it installs MSYS2 (via winget) and builds it itself.
2. Drag your Gothic folder (the one with Data\ and _work\) onto bake.bat.
3. Take out\data.pak (and music.pak / speech.pak if made) and copy to the SD card: 3ds\gothic3ds\

Music: render the WAVs once (docs/BUILD_WSL.md, "Music") and put them in windows\music_wavs\ - bake.bat uses them.
Gothic II / mods: bake.bat "C:\path\to\Gothic II" "--mod Archolos"
The reference-interpreter script check is skipped on Windows (it needs fork); it is a developer's check, not needed for play.
The .3dsx is built by the developer (it comes in the zip); to build it yourself you still need devkitPro (docs/BUILD_WSL.md).

Windows: запекание data.pak без WSL
====================================
Нужна только своя копия Gothic.
1. Положите g3ds-bake.exe (из релиза / артефакта CI "g3ds-bake-windows") рядом с bake.bat.
   Нет .exe? Один раз запустите build_baker.bat: он сам поставит MSYS2 (через winget) и соберёт пекаря.
2. Перетащите папку Gothic (где Data\ и _work\) на bake.bat.
3. Скопируйте out\data.pak (и music.pak / speech.pak) на SD-карту: 3ds\gothic3ds\
Музыка: WAV рендерятся один раз (docs/BUILD_WSL.md), положите их в windows\music_wavs\ - bake.bat подхватит.
Мод/Gothic II: bake.bat "C:\путь\Gothic II" "--mod Archolos"
Сверка скриптов с эталонным интерпретатором на Windows пропускается (нужен fork) - это проверка разработчика, для игры не нужна.
