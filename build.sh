#!/bin/sh
# Cross-compile CookieBot from Linux: sudo apt install g++-mingw-w64-x86-64-posix
set -e
cd "$(dirname "$0")/src"
x86_64-w64-mingw32-windres cookiebot.rc -O coff -o cookiebot.res
x86_64-w64-mingw32-g++-posix -O2 -s -std=c++17 -static -mwindows -o ../CookieBot.exe main.cpp cookiebot.res \
    -lruntimeobject -lwinmm -lole32 -luser32 -lgdi32 -lwininet -lcomdlg32 -luxtheme
cp ../CookieBot.exe ../CookieBot.ex_
echo "Built CookieBot.exe (and CookieBot.ex_ for the repo)"
