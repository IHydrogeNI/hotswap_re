@echo off
rem Builds bin\x86\hotswap_re.exe (Windows XP and later, 32-bit) and
rem bin\x64\hotswap_re.exe (Windows XP x64 / Vista and later, 64-bit).
rem
rem Needs llvm-mingw (https://github.com/mstorsjo/llvm-mingw, "msvcrt" build).
rem Set LLVM_MINGW to its folder, or put its bin folder on PATH.
rem The produced executables link no C runtime, only Windows system DLLs.

setlocal
if defined LLVM_MINGW set "PATH=%LLVM_MINGW%\bin;%PATH%"
cd /d "%~dp0"

call :build i686   x86 5.01 1 "-march=i686" || exit /b 1
call :build x86_64 x64 5.02 2 ""            || exit /b 1
echo Build OK.
exit /b 0

:build
set T=%1-w64-mingw32
set OUT=bin\%2
if not exist %OUT% mkdir %OUT%
for /f "delims=" %%L in ('%T%-clang -print-libgcc-file-name') do set "BUILTINS=%%L"
%T%-windres -I src src\hotswap_re.rc -O coff -o %OUT%\res.o || exit /b 1
%T%-clang -Os -Wall -Wno-missing-braces -ffreestanding -fno-builtin -fno-stack-protector %~5 ^
    -nostdlib -e entry ^
    -Wl,--subsystem,windows:%3 -Wl,--major-os-version,5 -Wl,--minor-os-version,%4 ^
    -Wl,--major-image-version,1 -s ^
    src\hotswap_re.c src\rt.c %OUT%\res.o ^
    -lkernel32 -luser32 -lshell32 -ladvapi32 -lsetupapi -lole32 -loleaut32 "%BUILTINS%" ^
    -o %OUT%\hotswap_re.exe || exit /b 1
del %OUT%\res.o
exit /b 0
