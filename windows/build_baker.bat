@echo off
rem Builds g3ds-bake.exe once (about 10-15 minutes the first time). Needs internet.
rem Installs MSYS2 (a free compiler kit) if it is missing; nothing else.
setlocal
set "HERE=%~dp0"
set "MSYS=C:\msys64"
if not exist "%MSYS%\usr\bin\bash.exe" (
  echo MSYS2 not found. Installing it with winget...
  winget install -e --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements
  if not exist "%MSYS%\usr\bin\bash.exe" (
    echo.
    echo Could not install MSYS2 automatically. Install it from https://www.msys2.org
    echo into C:\msys64 and run this file again.
    pause
    exit /b 1
  )
)
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
"%MSYS%\usr\bin\bash.exe" -lc "set -e; cd \"$(cygpath -u '%HERE%')/..\"; pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja git; cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DG3DS_BAKER_ONLY=ON; cmake --build build-win --target g3ds-bake -j$(nproc); cp build-win/g3ds-bake.exe windows/g3ds-bake.exe"
if errorlevel 1 (
  echo.
  echo The build failed. Send the text above to the developer.
  pause
  exit /b 1
)
echo.
echo Done: windows\g3ds-bake.exe. Now run bake.bat.
pause
