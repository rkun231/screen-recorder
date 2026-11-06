#!/bin/bash
set -e
cd "$(dirname "$0")"
mkdir -p out
x86_64-w64-mingw32-windres -c 65001 res/app.rc -O coff -o out/app.res.o --include-dir res
x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -municode -mwindows -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -finput-charset=UTF-8 -Wall -Wno-missing-field-initializers \
  src/main.cpp src/audio.cpp out/app.res.o -o out/Recorder.exe \
  -static -static-libgcc -static-libstdc++ \
  -lole32 -luuid -lcomctl32 -lshell32 -lshlwapi -ldxgi -lgdi32 -luser32 -lshcore -loleaut32 -lpropsys -ldwmapi -lavrt
x86_64-w64-mingw32-strip out/Recorder.exe
ls -la out/Recorder.exe
