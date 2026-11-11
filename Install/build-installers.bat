@echo off
setlocal
cd /d "%~dp0.."
if not exist build\arm64\ZImager.exe (
  echo build\arm64\ZImager.exe is missing. Run build.bat first.
  exit /b 1
)
if not exist build\x64\ZImager.exe (
  echo build\x64\ZImager.exe is missing. Run build-x64.bat first.
  exit /b 1
)
if not exist dist mkdir dist
set ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" (
  echo Inno Setup 6 ISCC.exe was not found.
  exit /b 1
)
"%ISCC%" /DArch=arm64 "%~dp0ZImager.iss"
if errorlevel 1 exit /b 1
"%ISCC%" /DArch=x64 "%~dp0ZImager.iss"
if errorlevel 1 exit /b 1
echo Installers are in dist\
exit /b 0
