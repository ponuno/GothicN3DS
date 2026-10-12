@echo off
rem Builds g3ds-bake.exe once (10-15 minutes). Needs internet. Installs MSYS2 (a free compiler kit) if missing.
rem "build_baker.bat nopause" - for calls from bake.bat.
setlocal
set "HERE=%~dp0"
set "MSYS=C:\msys64"
rem Variables of other compilers on this PC (CXX=C:\MinGW\bin ...) would break the build: forget them.
set CC=
set CXX=
set CFLAGS=
set CXXFLAGS=
set LDFLAGS=
set CPATH=
set LIBRARY_PATH=
set CMAKE_GENERATOR=
set CMAKE_PREFIX_PATH=
if not exist "%MSYS%\usr\bin\bash.exe" (
  echo MSYS2 not found. Installing it with winget...
  winget install -e --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements
  if not exist "%MSYS%\usr\bin\bash.exe" (
    echo.
    echo Could not install MSYS2 automatically. Install it from https://www.msys2.org
    echo into C:\msys64 and run this file again.
    if not "%1"=="nopause" pause
    exit /b 1
  )
)
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
set "SCRIPT=%HERE%build_baker.sh"
"%MSYS%\usr\bin\bash.exe" -lc "bash \"$(cygpath -u '%SCRIPT%')\""
if errorlevel 1 (
  echo.
  echo The build failed. Run this file again ^(a slow download is retried^), or send the text above to the developer.
  if not "%1"=="nopause" pause
  exit /b 1
)
echo.
echo Done: windows\g3ds-bake.exe
if not "%1"=="nopause" pause
