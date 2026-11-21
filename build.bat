@echo off
rem Build CookieBot with MSYS2 MinGW-w64 (run from an MSYS2 MinGW64 shell or with its bin folder on PATH)
cd /d "%~dp0src"
windres cookiebot.rc -O coff -o cookiebot.res || exit /b 1
g++ -O2 -s -std=c++17 -static -mwindows -o ..\CookieBot.exe main.cpp cookiebot.res -lruntimeobject -lwinmm -lole32 -luser32 -lgdi32 -lwininet -lcomdlg32 -luxtheme || exit /b 1
copy /y ..\CookieBot.exe ..\CookieBot.ex_ >nul
echo Built CookieBot.exe (and CookieBot.ex_ for the repo)
