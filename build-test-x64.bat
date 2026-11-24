@echo off
setlocal
rem Same grade-math test as build-test.bat, compiled for x64.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere was not found.
  exit /b 1
)
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo The x64 C tools were not found.
  exit /b 1
)
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" arm64_x64
) else (
  call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64
)
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build\x64 mkdir build\x64
cl /nologo /W4 /std:c11 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fe:build\x64\test_features.exe /Fo:build\x64\ src\test_features.c src\features.c
if errorlevel 1 exit /b 1
build\x64\test_features.exe
exit /b %ERRORLEVEL%
