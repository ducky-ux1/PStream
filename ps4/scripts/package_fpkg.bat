@echo off
setlocal enabledelayedexpansion

pushd "%~dp0.."

if "%OO_PS4_TOOLCHAIN%"=="" set "OO_PS4_TOOLCHAIN=C:\OpenOrbis\OpenOrbis\PS4Toolchain"
set "PKG_TOOL=%OO_PS4_TOOLCHAIN%\bin\windows\PkgTool.Core.exe"

set "CONTENT_ID=IV0001-PSTR00004_00-PSTREAMPS4000001"
set "TITLE_ID=PSTR00004"
set "TITLE=PStream"
set "VERSION=01.10"
set "OUT_PKG=PStream-PS4-v1.1.0.pkg"

echo =========================================================================
echo   Packaging PStream PS4 Standalone Fake PKG (v1.1.0)
echo =========================================================================

echo [1/4] Preparing package staging directory...
if exist "build\pkg_staging" rmdir /S /Q "build\pkg_staging"
mkdir "build\pkg_staging\sce_sys\about"
mkdir "build\pkg_staging\sce_module"
mkdir "build\pkg_staging\www"

copy /Y "build\eboot.bin" "build\pkg_staging\eboot.bin" >nul
copy /Y "sce_sys\icon0.png" "build\pkg_staging\sce_sys\icon0.png" >nul
copy /Y "sce_sys\pic1.png" "build\pkg_staging\sce_sys\pic1.png" >nul
copy /Y "sce_sys\about\right.sprx" "build\pkg_staging\sce_sys\about\right.sprx" >nul
copy /Y "sce_module\libc.prx" "build\pkg_staging\sce_module\libc.prx" >nul
copy /Y "sce_module\libSceFios2.prx" "build\pkg_staging\sce_module\libSceFios2.prx" >nul

for %%F in (index.html app.js app.css input.js hls.min.js logo.png favicon.png version.json) do (
    copy /Y "..\ps5\www\%%F" "build\pkg_staging\www\%%F" >nul
)

echo [2/4] Generating param.sfo...
"%PKG_TOOL%" sfo_new "build\pkg_staging\sce_sys\param.sfo" >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" APP_TYPE --type Integer --maxsize 4 --value 1 >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" APP_VER --type Utf8 --maxsize 8 --value %VERSION% >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" ATTRIBUTE --type Integer --maxsize 4 --value 0 >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" CATEGORY --type Utf8 --maxsize 4 --value gd >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" CONTENT_ID --type Utf8 --maxsize 48 --value %CONTENT_ID% >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0 >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" SYSTEM_VER --type Integer --maxsize 4 --value 0 >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" TITLE --type Utf8 --maxsize 128 --value "%TITLE%" >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" TITLE_ID --type Utf8 --maxsize 12 --value %TITLE_ID% >nul
"%PKG_TOOL%" sfo_setentry "build\pkg_staging\sce_sys\param.sfo" VERSION --type Utf8 --maxsize 8 --value %VERSION% >nul

echo [3/4] Generating GP4 project descriptor (pkg.gp4)...
(
echo ^<?xml version="1.0"?^>
echo ^<psproject xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" fmt="gp4" version="1000"^>
echo 	^<volume^>
echo 		^<volume_type^>pkg_ps4_app^</volume_type^>
echo 		^<volume_id^>PS4VOLUME^</volume_id^>
echo 		^<volume_ts^>2026-11-07 18:00:00^</volume_ts^>
echo 		^<package content_id="%CONTENT_ID%" passcode="00000000000000000000000000000000"
echo 			storage_type="digital50" app_type="full" /^>
echo 		^<chunk_info chunk_count="1" scenario_count="1"^>
echo 			^<chunks^>
echo 				^<chunk id="0" layer_no="0" label="Chunk #0" /^>
echo 			^</chunks^>
echo 			^<scenarios default_id="0"^>
echo 				^<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0"^>0^</scenario^>
echo 			^</scenarios^>
echo 		^</chunk_info^>
echo 	^</volume^>
echo 	^<files img_no="0"^>
echo 		^<file targ_path="eboot.bin" orig_path="eboot.bin" /^>
echo 		^<file targ_path="sce_sys/param.sfo" orig_path="sce_sys/param.sfo" /^>
echo 		^<file targ_path="sce_sys/icon0.png" orig_path="sce_sys/icon0.png" /^>
echo 		^<file targ_path="sce_sys/pic1.png" orig_path="sce_sys/pic1.png" /^>
echo 		^<file targ_path="sce_sys/about/right.sprx" orig_path="sce_sys/about/right.sprx" /^>
echo 		^<file targ_path="sce_module/libc.prx" orig_path="sce_module/libc.prx" /^>
echo 		^<file targ_path="sce_module/libSceFios2.prx" orig_path="sce_module/libSceFios2.prx" /^>
echo 		^<file targ_path="www/index.html" orig_path="www/index.html" /^>
echo 		^<file targ_path="www/app.js" orig_path="www/app.js" /^>
echo 		^<file targ_path="www/app.css" orig_path="www/app.css" /^>
echo 		^<file targ_path="www/input.js" orig_path="www/input.js" /^>
echo 		^<file targ_path="www/hls.min.js" orig_path="www/hls.min.js" /^>
echo 		^<file targ_path="www/logo.png" orig_path="www/logo.png" /^>
echo 		^<file targ_path="www/favicon.png" orig_path="www/favicon.png" /^>
echo 		^<file targ_path="www/version.json" orig_path="www/version.json" /^>
echo 	^</files^>
echo 	^<rootdir^>
echo 		^<dir targ_name="sce_sys"^>
echo 			^<dir targ_name="about" /^>
echo 		^</dir^>
echo 		^<dir targ_name="sce_module" /^>
echo 		^<dir targ_name="www" /^>
echo 	^</rootdir^>
echo ^</psproject^>
) > "build\pkg_staging\pkg.gp4"

echo [4/4] Building and validating Fake PKG...
pushd "build\pkg_staging"
"%PKG_TOOL%" pkg_build pkg.gp4 .
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] pkg_build failed with exit code %ERRORLEVEL%
    popd & popd & exit /b %ERRORLEVEL%
)
popd

copy /Y "build\pkg_staging\%CONTENT_ID%.pkg" "build\%OUT_PKG%" >nul
"%PKG_TOOL%" pkg_validate "build\%OUT_PKG%"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] pkg_validate failed!
    popd & exit /b %ERRORLEVEL%
)

echo =========================================================================
echo   SUCCESS: ps4\build\%OUT_PKG%
echo =========================================================================
popd
