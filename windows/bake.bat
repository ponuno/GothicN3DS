@echo off
rem Bakes data.pak from your Gothic folder.
rem Drag the Gothic folder onto this file, or run it and type the path.
setlocal
chcp 65001 >nul
set "HERE=%~dp0"
if not exist "%HERE%g3ds-bake.exe" (
  echo g3ds-bake.exe is not here yet: building it now ^(once, 10-15 minutes, needs internet^)...
  call "%HERE%build_baker.bat" nopause
  if not exist "%HERE%g3ds-bake.exe" (
    pause
    exit /b 1
  )
)
set "GAME=%~1"
if "%GAME%"=="" set /p "GAME=Path to the Gothic folder (the one with Data and _work): "
set "GAME=%GAME:"=%"
if not exist "%GAME%\Data" (
  echo "%GAME%\Data" not found. Give the folder that contains Data\ and _work\.
  pause
  exit /b 1
)
set "OUT=%HERE%out"
if not exist "%OUT%" mkdir "%OUT%"
rem Optional: a folder of rendered music WAVs next to this file: music_wavs
set "MUSIC="
if exist "%HERE%music_wavs" set MUSIC=--music-dir "%HERE%music_wavs"
rem Extra flags (for example --mod Archolos) can be given as the second argument.
echo Baking... this takes a while (the window may look idle).
"%HERE%g3ds-bake.exe" world --game "%GAME%" -o "%OUT%\data.pak" --no-script-check %MUSIC% %~2 > "%OUT%\bake.log" 2>&1
if errorlevel 1 (
  echo FAILED. See %OUT%\bake.log
  type "%OUT%\bake.log" | more
  pause
  exit /b 1
)
echo.
echo Done: %OUT%\data.pak  ^(and music.pak / speech.pak if they were made^)
echo Copy them to the SD card: SD:\3ds\gothic3ds\  next to gothic3ds.3dsx
pause
