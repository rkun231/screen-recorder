#!/bin/bash
# Builds 편집기프로그램/Editor.exe (cross-compiled with mingw-w64).
set -e
cd "$(dirname "$0")"
mkdir -p out
x86_64-w64-mingw32-windres -c 65001 res_editor/editor.rc -O coff -o out/editor.res.o --include-dir res_editor
x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -municode -mwindows -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -finput-charset=UTF-8 -Wall -Wno-missing-field-initializers \
  editor/editor.cpp editor/media.cpp editor/player.cpp out/editor.res.o -o out/Editor.exe \
  -static -static-libgcc -static-libstdc++ \
  -lole32 -luuid -lcomctl32 -lshell32 -lshlwapi -lgdi32 -luser32 -lshcore -loleaut32 -lcomdlg32
x86_64-w64-mingw32-strip out/Editor.exe
ls -la out/Editor.exe
