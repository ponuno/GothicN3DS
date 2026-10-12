Windows: baking data.pak without WSL
====================================
Needs only your own copy of Gothic.

1. Run Gothic3DS-Baker.exe (it needs g3ds-bake.exe and dmrender.exe beside it).
   If you have no .exe files yet: run build_baker.bat once (it installs MSYS2 via winget and builds them, 10-15 minutes, needs internet).
2. In the window pick the Gothic folder (the one with Data\ and _work\) and press "Запечь" (Bake).
   Tick the music box to bake the music too (10-30 minutes more).
3. data.pak, music.pak and speech.pak appear beside the .exe (and bake.log). Copy them to the SD card: 3ds\gothic3ds\
   Also needed on the console: dspfirm.cdc in 3ds\ (see docs/BUILD_WSL.md).

Command line (no window): bake.bat "C:\path\to\Gothic" ["--mod Archolos"]
Reference-interpreter script check is skipped on Windows (a developer's check, not needed for play).

Windows: запекание data.pak без WSL
====================================
Нужна только своя копия Gothic.
1. Запустите Gothic3DS-Baker.exe (рядом должны лежать g3ds-bake.exe и dmrender.exe).
   Нет .exe? Один раз запустите build_baker.bat: он сам поставит MSYS2 (через winget) и соберёт их (10-15 минут, нужен интернет).
2. В окне выберите папку Gothic (где Data\ и _work\) и нажмите «Запечь». Галочка «музыка» — запечь и музыку (ещё 10-30 минут).
3. data.pak, music.pak и speech.pak появятся рядом с .exe (и bake.log). Скопируйте их на SD-карту: 3ds\gothic3ds\
   На консоли нужен ещё dspfirm.cdc в папке 3ds\ (см. docs/BUILD_WSL.md).
Без окна: bake.bat "C:\путь\Gothic" ["--mod Archolos"]
