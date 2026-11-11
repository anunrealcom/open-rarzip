@echo off
rem One-click Windows packaging for Yasuo Compressor.
rem Usage: scripts\package.bat [build_dir] [qt_prefix]
rem   build_dir  defaults to build\v2 (must contain YasuoCompressor.exe)
rem   qt_prefix  defaults to the Qt6_DIR recorded in that build's CMakeCache.txt
rem Output: dist\YasuoCompressor-2.0.0-Setup.exe

setlocal enabledelayedexpansion
cd /d "%~dp0.." || exit /b 1

set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build\v2"
set "QT_PREFIX=%~2"
set "DEPLOY=%CD%\build\deploy"
set "EXE=YasuoCompressor.exe"

if not exist "%BUILD_DIR%\%EXE%" (
  echo [ERROR] "%BUILD_DIR%\%EXE%" not found. Build the project first.
  exit /b 1
)

if "%QT_PREFIX%"=="" (
  set "CACHE=%BUILD_DIR%\CMakeCache.txt"
  if exist "!CACHE!" (
    for /f "usebackq tokens=1,* delims==" %%A in (`findstr /B /C:"Qt6_DIR:" "!CACHE!"`) do set "QT6_DIR=%%B"
    if defined QT6_DIR for %%P in ("!QT6_DIR!\..\..\..") do set "QT_PREFIX=%%~fP"
  )
)
if "%QT_PREFIX%"=="" (
  for /d %%R in ("D:\Qt\*" "C:\Qt\*" "E:\Qt\*") do (
    if exist "%%~fR\mingw_64\bin\windeployqt.exe" set "QT_PREFIX=%%~fR\mingw_64"
  )
)
if "%QT_PREFIX%"=="" (
  echo [ERROR] Cannot locate Qt. Pass the Qt prefix as the second argument.
  exit /b 1
)
set "WDQT=%QT_PREFIX%\bin\windeployqt.exe"
if not exist "%WDQT%" (
  echo [ERROR] windeployqt not found at "%WDQT%".
  exit /b 1
)

echo [1/4] Preparing "%DEPLOY%"
if exist "%DEPLOY%" rmdir /s /q "%DEPLOY%"
mkdir "%DEPLOY%" || exit /b 1
copy /y "%BUILD_DIR%\%EXE%" "%DEPLOY%\%EXE%" >nul || exit /b 1

echo [2/4] Deploying Qt runtime with "%WDQT%"
"%WDQT%" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%DEPLOY%\%EXE%"
if errorlevel 1 (
  echo [ERROR] windeployqt failed.
  exit /b 1
)

set "ISCC_EXE=%ISCC%"
if "%ISCC_EXE%"=="" (
  for %%P in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 5\ISCC.exe") do (
    if exist %%P set "ISCC_EXE=%%~P"
  )
)
if "%ISCC_EXE%"=="" (
  echo [WARN] Inno Setup ISCC.exe not found. Qt runtime is ready at "%DEPLOY%".
  echo        Set ISCC to the compiler path, then run ISCC on "%CD%\installer\setup.iss"
  exit /b 0
)

echo [3/4] Compiling installer
if not exist "%CD%\dist" mkdir "%CD%\dist"
"%ISCC_EXE%" "%CD%\installer\setup.iss"
if errorlevel 1 (
  echo [ERROR] ISCC failed.
  exit /b 1
)

echo [4/4] Done. Installer is in "%CD%\dist"
exit /b 0
