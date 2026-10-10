#!/bin/sh
# Checks the console's code without devkitARM: compiles every file of the
# .3dsx with clang for ARM against the headers of newlib, libctru and citro3d.
#
#   scripts/check_3ds.sh <dir with newlib, libctru and citro3d checked out>
#
# It is no replacement for `make` in the devkitPro container - nothing is
# linked - but it finds what the PC build cannot: code that only the console
# compiles, and the console's types. The second pass matters most:
#
#   devkitARM's GCC makes int32_t `long` and uint32_t `unsigned long`
#   (size_t stays `unsigned int`), where clang and every PC make them `int`.
#   So clamp(i32, 0, 5), min(u32, sizeof x) or passing a u32* for an
#   unsigned* compile everywhere except on the console. The second pass
#   gives clang the console's types.
set -u
deps=${1:?usage: check_3ds.sh <dir with newlib, libctru, citro3d>}
cd "$(dirname "$0")/.." || exit 2
cxx=${CLANGXX:-clang++}
common="--target=arm-none-eabi -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffunction-sections
 -fno-rtti -fno-exceptions -std=gnu++17 -Wall -Wextra -Wshadow -Wframe-larger-than=4096
 -D__3DS__ -DARM11 -D_3DS -nostdlibinc
 -isystem $deps/newlib/newlib/libc/include -isystem $deps/libctru/libctru/include -isystem $deps/citro3d/include
 -Icore/include -Iengine -Iplatform/3ds -Ithird_party/lz4 -fsyntax-only"
failed=0
for f in core/src/*.cpp engine/*.cpp platform/3ds/*.cpp; do
	# 1: as clang sees the console.
	out=$($cxx $common "$f" 2>&1)
	[ -n "$out" ] && { echo "$out"; failed=1; }
	# 2: with the integer types of devkitARM's GCC. -Wformat is left to the
	# first pass: clang checks printf against its own idea of the types.
	out=$($cxx $common -Wno-builtin-macro-redefined -Wno-format \
		-U__INT32_TYPE__ "-D__INT32_TYPE__=long int" -U__UINT32_TYPE__ "-D__UINT32_TYPE__=long unsigned int" \
		-U__INT_LEAST32_TYPE__ "-D__INT_LEAST32_TYPE__=long int" \
		-U__UINT_LEAST32_TYPE__ "-D__UINT_LEAST32_TYPE__=long unsigned int" "$f" 2>&1)
	[ -n "$out" ] && { echo "== with the console's integer types: $f"; echo "$out"; failed=1; }
done
[ $failed = 0 ] && echo "check_3ds: clean"
exit $failed
