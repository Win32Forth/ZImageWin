@echo off
setlocal
rem ARM64 is the first target. The sources are the same ones build-x64.bat compiles.
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
rc /nologo /fo build\arm64\ZImager.res src\ZImager.rc
if errorlevel 1 exit /b 1
cl /nologo /W4 /std:c11 /O2 /utf-8 /DUNICODE /D_UNICODE /Fe:build\arm64\ZImager.exe /Fo:build\arm64\ src\app.c src\chrome.c src\helper.c src\grid.c src\library.c src\editor.c src\tone.c src\touchup.c src\blake3.c src\zimg.c build\arm64\ZImager.res /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\ZImager.manifest user32.lib gdi32.lib ole32.lib shell32.lib uuid.lib winhttp.lib bcrypt.lib crypt32.lib mpr.lib windowscodecs.lib oleaut32.lib comdlg32.lib msimg32.lib
if errorlevel 1 exit /b 1
echo Built build\arm64\ZImager.exe
exit /b 0
