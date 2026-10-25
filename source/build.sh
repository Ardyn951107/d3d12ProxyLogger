#!/bin/sh
# Builds d3d12.dll (x64) with mingw-w64 on Linux/WSL.
#   sudo apt install g++-mingw-w64-x86-64 python3 git
#   ./build.sh            -> ../bin/d3d12.dll
set -e
cd "$(dirname "$0")"
HDR=${DXHEADERS:-./DirectX-Headers}
[ -d "$HDR" ] || git clone --depth 1 https://github.com/microsoft/DirectX-Headers.git "$HDR"
INC="$HDR/include/directx"
CXX=x86_64-w64-mingw32-g++-win32; command -v $CXX >/dev/null || CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc-win32;  command -v $CC  >/dev/null || CC=x86_64-w64-mingw32-gcc
python3 gen_names.py "$INC" names_gen.h
$CC -I"$INC" -fsyntax-only vtbl_check.c               # vtable slot check
mkdir -p ../bin
$CXX -std=c++17 -O2 -Wall -Wno-unknown-pragmas -Wno-missing-braces -I"$INC" -I. \
     -fno-exceptions -fno-rtti -shared \
     -o ../bin/d3d12.dll d3d12proxy.cpp d3d12proxy.def \
     -static -static-libgcc -static-libstdc++ -s \
     -Wl,--enable-stdcall-fixup -Wl,--kill-at -Wl,--image-base,0x180000000 \
     -lkernel32 -ladvapi32 -luuid
echo "built ../bin/d3d12.dll"
