@echo off
setlocal
rem Headless grade math. ARM64 first. The x64 bat compiles the same two files.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere was not found.
  exit /b 1
)
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo The ARM64 C tools were not found.
  exit /b 1
)
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" arm64
) else (
  call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64
)
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build\arm64 mkdir build\arm64
cl /nologo /W4 /std:c11 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fe:build\arm64\test_features.exe /Fo:build\arm64\ src\test_features.c src\features.c
if errorlevel 1 exit /b 1
build\arm64\test_features.exe
exit /b %ERRORLEVEL%
