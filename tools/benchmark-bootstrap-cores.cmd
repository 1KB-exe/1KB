@echo off
setlocal EnableDelayedExpansion
pushd "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=%CD%\ignore\1KB-core-build"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || goto :fail

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto :fail
call "%VSROOT%\VC\Auxiliary\Build\vcvars32.bat" >nul || goto :fail

py -3 tools\write-runtime-url-inc.py src\Config.h "%OUT%\runtime-url.inc" || goto :fail
ml.exe /nologo /c /coff /I"%OUT%" /Fo"%OUT%\gui.obj" src\bootstrap-x86.asm || goto :fail
ml.exe /nologo /c /coff /I"%OUT%" /DONEKB_CONSOLE_BOOTSTRAP /Fo"%OUT%\console.obj" src\bootstrap-x86.asm || goto :fail
set "OPTIONS=/CRINKLER /ENTRY:BootstrapEntry /NODEFAULTLIB /UNALIGNCODE /OVERRIDEALIGNMENTS /TINYHEADER /TINYIMPORT /ORDERTRIES:1000"
for %%S in (gui console) do (
  if "%%S"=="gui" (set "SUBSYSTEM=WINDOWS") else (set "SUBSYSTEM=CONSOLE")
  third_party\crinkler-bootstrap\Win32\Crinkler.exe !OPTIONS! /OUT:"%OUT%\%%S.exe" /REPORT:"%OUT%\%%S.html" /SUBSYSTEM:!SUBSYSTEM! "%OUT%\%%S.obj" kernel32.lib urlmon.lib || goto :fail
  third_party\crinkler-icon\Win32\Crinkler.exe !OPTIONS! /OUT:"%OUT%\%%S-icon.exe" /REPORT:"%OUT%\%%S-icon.html" /SUBSYSTEM:!SUBSYSTEM! "%OUT%\%%S.obj" kernel32.lib urlmon.lib || goto :fail
)
echo.
for %%F in ("%OUT%\gui.exe" "%OUT%\console.exe" "%OUT%\gui-icon.exe" "%OUT%\console-icon.exe") do echo %%~nxF %%~zF bytes
popd
exit /b 0
:fail
echo ERROR: Core benchmark failed.
popd
exit /b 1
