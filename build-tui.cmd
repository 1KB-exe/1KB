@echo off
setlocal
pushd "%~dp0"

rem Fast developer build for testing the manager TUI. This intentionally skips
rem the runtime, x86 bootstraps, embedded launcher templates, Crinkler, and UPX.
where cl.exe >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
)
if not defined VCVARS if defined VSROOT set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS call "%VCVARS%" >nul

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: The Visual C++ compiler was not found.
  echo Install Visual Studio 2022 Build Tools with "Desktop development with C++".
  goto :failed
)

set "BUILD=%CD%\ignore\tui-build"
if not exist "%BUILD%" mkdir "%BUILD%" || goto :failed
set "CPP=/nologo /c /MP /std:c++17 /utf-8 /permissive- /W4 /wd4505 /sdl /Od /GS /GR- /EHsc /MD /DUNICODE /D_UNICODE /D_HAS_EXCEPTIONS=0 /D_ITERATOR_DEBUG_LEVEL=0 /DONEKB_BUILDER_ONLY"
set "C=/nologo /c /MP /W4 /wd4127 /wd4242 /wd4244 /wd4267 /wd4702 /Od /GS /MD"

echo Compiling fast TUI test build...
pushd "%BUILD%"
cl.exe %C% ..\..\third_party\zlib\adler32.c ..\..\third_party\zlib\compress.c ..\..\third_party\zlib\crc32.c ..\..\third_party\zlib\deflate.c ..\..\third_party\zlib\trees.c ..\..\third_party\zlib\zutil.c || (popd & goto :failed)
cl.exe %CPP% ..\..\src\icon-png-optimizer.cpp ..\..\src\overlay-identity.cpp ..\..\src\icon-crinkler-packer.cpp ..\..\src\deployment-manager.cpp ..\..\src\payload-crypto.cpp ..\..\src\main.cpp || (popd & goto :failed)
popd

cl.exe /nologo "%BUILD%\main.obj" "%BUILD%\deployment-manager.obj" "%BUILD%\overlay-identity.obj" "%BUILD%\payload-crypto.obj" "%BUILD%\icon-png-optimizer.obj" "%BUILD%\icon-crinkler-packer.obj" "%BUILD%\adler32.obj" "%BUILD%\compress.obj" "%BUILD%\crc32.obj" "%BUILD%\deflate.obj" "%BUILD%\trees.obj" "%BUILD%\zutil.obj" /Fe"%CD%\ignore\1KB-tui.exe" /link winhttp.lib shell32.lib ole32.lib user32.lib bcrypt.lib /SUBSYSTEM:CONSOLE /INCREMENTAL:NO || goto :failed

echo Built TUI test executable: %CD%\ignore\1KB-tui.exe
echo Starting TUI test executable...
"%CD%\ignore\1KB-tui.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

:failed
echo ERROR: TUI test build failed. Review the error above.
popd
exit /b 1
