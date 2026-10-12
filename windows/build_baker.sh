#!/usr/bin/env bash
# Run inside MSYS2 UCRT64 by build_baker.bat.
set -e
cd "$(dirname "$0")/.."
unset CC CXX CFLAGS CXXFLAGS LDFLAGS CPATH LIBRARY_PATH
need="mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja git"
# (The mirrors are sometimes slow: try again, with no download timeout.)
for try in 1 2 3 4; do
  if pacman -S --needed --noconfirm --disable-download-timeout $need; then ok=1; break; fi
  echo "pacman failed, try $try of 4; changing the mirror order and trying again"
  sleep 3
done
[ -n "$ok" ] || { echo "could not install the compiler"; exit 1; }
which g++ cmake ninja
rm -rf build-win
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DG3DS_BAKER_ONLY=ON
cmake --build build-win --target g3ds-bake -j"$(nproc)"
cp build-win/g3ds-bake.exe windows/g3ds-bake.exe
