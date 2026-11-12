@echo off
setlocal
cd /d "%~dp0"
if not defined CXX set "CXX=clang++"
if not defined WINDRES set "WINDRES=llvm-windres"
if not exist build mkdir build
"%WINDRES%" VapeHub.rc -O coff -o build\VapeHub.res.o || exit /b 1
"%CXX%" -std=c++17 -O2 -mwindows -static -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 main.cpp build\VapeHub.res.o -o ..\VapeHub.exe -lws2_32 -lshell32 -lshlwapi -lole32 -lgdi32 -luser32 -ladvapi32 || exit /b 1
echo built %~dp0..\VapeHub.exe
