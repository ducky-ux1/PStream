@echo off
setlocal enabledelayedexpansion

pushd "%~dp0.."

if "%OO_PS4_TOOLCHAIN%"=="" set "OO_PS4_TOOLCHAIN=C:\OpenOrbis\OpenOrbis\PS4Toolchain"
if "%CLANG%"=="" set "CLANG=C:\OpenOrbis\clang+llvm-18.1.0-x86_64-pc-windows-msvc\bin\clang++.exe"
if "%LLD%"=="" set "LLD=C:\OpenOrbis\clang+llvm-18.1.0-x86_64-pc-windows-msvc\bin\ld.lld.exe"
set "CREATE_FSELF=%OO_PS4_TOOLCHAIN%\bin\windows\create-fself.exe"

set "INCLUDES=-Iinclude -I"%OO_PS4_TOOLCHAIN%\include" -I"%OO_PS4_TOOLCHAIN%\include\c++\v1""
set "FLAGS=--target=x86_64-pc-freebsd12-elf -std=gnu++17 -Wall -Werror -O2 -fPIC -funwind-tables"
set "LIBS=-lc -lkernel -lc++ -lSceHttp -lSceSsl -lSceNet -lSceNetCtl -lSceCommonDialog -lSceWebBrowserDialog -lSceSysmodule -lSceUserService -lSceVideoOut -lSceSystemService"

echo [1/4] Preparing PS4 build directories...
if not exist "build\obj" mkdir "build\obj"

echo [2/4] Compiling PStream PS4 C++ sources...
"%CLANG%" %FLAGS% %INCLUDES% -c src/graphics.cpp -o build/obj/graphics.o
if %ERRORLEVEL% NEQ 0 (popd & exit /b %ERRORLEVEL%)

"%CLANG%" %FLAGS% %INCLUDES% -c src/server.cpp -o build/obj/server.o
if %ERRORLEVEL% NEQ 0 (popd & exit /b %ERRORLEVEL%)

"%CLANG%" %FLAGS% %INCLUDES% -c src/main.cpp -o build/obj/main.o
if %ERRORLEVEL% NEQ 0 (popd & exit /b %ERRORLEVEL%)

echo [3/4] Linking PStream PS4 eboot.elf...
"%LLD%" -m elf_x86_64 -pie --script "%~dp0link.x" --eh-frame-hdr -o "build/eboot.elf" "-L%OO_PS4_TOOLCHAIN%\lib" %LIBS% "%OO_PS4_TOOLCHAIN%\lib\crt1.o" build/obj/graphics.o build/obj/server.o build/obj/main.o
if %ERRORLEVEL% NEQ 0 (popd & exit /b %ERRORLEVEL%)

echo [4/4] Generating Orbis FSELF eboot.bin...
"%CREATE_FSELF%" -in "build\eboot.elf" --out "build\eboot.oelf" --eboot "build\eboot_raw.bin" --paid 0x3800000000000011
if %ERRORLEVEL% NEQ 0 (popd & exit /b %ERRORLEVEL%)
py "%~dp0clean_oelf.py" "build\eboot.oelf" "build\eboot_clean.oelf"
if %ERRORLEVEL% NEQ 0 (popd & exit /b %ERRORLEVEL%)
py "%~dp0make_fself.py" --paid 0x3800000000000011 "build\eboot_clean.oelf" "build\eboot.bin"
if %ERRORLEVEL% NEQ 0 (popd & exit /b %ERRORLEVEL%)

echo =========================================================
echo   PStream PS4 Build Complete: ps4\build\eboot.bin
echo =========================================================
popd
