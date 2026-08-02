@echo off
setlocal
pushd "%~dp0"

rem Load the Visual C++ x64 command-line environment when cl.exe is not already available.
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
if not defined VCVARS if defined VCINSTALLDIR set "VCVARS=%VCINSTALLDIR%Auxiliary\Build\vcvars64.bat"
if defined VCVARS set "VCVARS32=%VCVARS:\vcvars64.bat=\vcvars32.bat%"
if not exist "%VCVARS32%" (
  echo ERROR: The Visual C++ x86 compiler environment was not found.
  goto :failed
)

set "TEMPBUILD=%CD%\.1KB-build"
if exist "%TEMPBUILD%" rmdir /s /q "%TEMPBUILD%"
mkdir "%TEMPBUILD%" || goto :failed
py -3 tools\write-runtime-url-inc.py src\Config.h "%TEMPBUILD%\runtime-url.inc" || goto :failed

set "RUNTIME=/MD"
if /I "%STATIC_ONEKB_RUNTIME%"=="1" set "RUNTIME=/MT"

set "COMMON=/nologo /std:c++17 /utf-8 /permissive- /W4 /wd4505 /sdl /O1 /Os /Oi /GL /Gy /Gw /GS /GF /GR- /EHsc %RUNTIME% /DUNICODE /D_UNICODE /D_HAS_EXCEPTIONS=0 /D_ITERATOR_DEBUG_LEVEL=0"
set "LINKCOMMON=winhttp.lib shell32.lib ole32.lib user32.lib bcrypt.lib /LTCG /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /INCREMENTAL:NO /DEBUG:NONE"

call "%VCVARS32%" >nul

echo Compiling downloadable x86 launcher runtime...
cl.exe /c src\overlay-identity.cpp %COMMON% /DONEKB_RUNTIME_ONLY /Fo"%TEMPBUILD%\overlay-identity-runtime.obj" || goto :failed
cl.exe /c src\payload-crypto.cpp %COMMON% /DONEKB_RUNTIME_ONLY /Fo"%TEMPBUILD%\payload-crypto-runtime.obj" || goto :failed
cl.exe src\main.cpp "%TEMPBUILD%\overlay-identity-runtime.obj" "%TEMPBUILD%\payload-crypto-runtime.obj" %COMMON% /DONEKB_RUNTIME_ONLY /Fo"%TEMPBUILD%\runtime.obj" /Fd"%TEMPBUILD%\runtime.pdb" /Fe"%TEMPBUILD%\runtime.exe" /link %LINKCOMMON% /SUBSYSTEM:WINDOWS || goto :failed

echo Assembling tiny x86 GUI bootstrap template...
ml.exe /nologo /c /coff /I"%TEMPBUILD%" /Fo"%TEMPBUILD%\bootstrap-gui.obj" src\bootstrap-x86.asm || goto :failed
link.exe /nologo "%TEMPBUILD%\bootstrap-gui.obj" /out:"%TEMPBUILD%\1KB-gui-template.exe" /entry:BootstrapEntry@0 /subsystem:windows,6.0 /machine:x86 /nodefaultlib /fixed /nxcompat /opt:ref /opt:icf /merge:.rdata=.text /merge:.data=.text kernel32.lib urlmon.lib || goto :failed

set "CRINKLER_OPTIONS=/CRINKLER /ENTRY:BootstrapEntry /NODEFAULTLIB /UNALIGNCODE /OVERRIDEALIGNMENTS /TINYHEADER /TINYIMPORT /ORDERTRIES:1000"
echo Crinkler-compressing x86 GUI bootstrap template...
third_party\crinkler-bootstrap\Win32\Crinkler.exe %CRINKLER_OPTIONS% /OUT:"%TEMPBUILD%\1KB-gui-crinkler.exe" /SUBSYSTEM:WINDOWS "%TEMPBUILD%\bootstrap-gui.obj" kernel32.lib urlmon.lib || goto :failed

echo Crinkler-compressing resource-capable x86 GUI core...
third_party\crinkler-icon\Win32\Crinkler.exe %CRINKLER_OPTIONS% /OUT:"%TEMPBUILD%\1KB-gui-icon-core.exe" /SUBSYSTEM:WINDOWS "%TEMPBUILD%\bootstrap-gui.obj" kernel32.lib urlmon.lib || goto :failed

echo Assembling tiny x86 console bootstrap template...
ml.exe /nologo /c /coff /I"%TEMPBUILD%" /DONEKB_CONSOLE_BOOTSTRAP /Fo"%TEMPBUILD%\bootstrap-console.obj" src\bootstrap-x86.asm || goto :failed
link.exe /nologo "%TEMPBUILD%\bootstrap-console.obj" /out:"%TEMPBUILD%\1KB-console-template.exe" /entry:BootstrapEntry@0 /subsystem:console,6.0 /machine:x86 /nodefaultlib /fixed /nxcompat /opt:ref /opt:icf /merge:.rdata=.text /merge:.data=.text kernel32.lib urlmon.lib || goto :failed

echo Crinkler-compressing x86 console bootstrap template...
third_party\crinkler-bootstrap\Win32\Crinkler.exe %CRINKLER_OPTIONS% /OUT:"%TEMPBUILD%\1KB-console-crinkler.exe" /SUBSYSTEM:CONSOLE "%TEMPBUILD%\bootstrap-console.obj" kernel32.lib urlmon.lib || goto :failed

echo Crinkler-compressing resource-capable x86 console core...
third_party\crinkler-icon\Win32\Crinkler.exe %CRINKLER_OPTIONS% /OUT:"%TEMPBUILD%\1KB-console-icon-core.exe" /SUBSYSTEM:CONSOLE "%TEMPBUILD%\bootstrap-console.obj" kernel32.lib urlmon.lib || goto :failed
call "%VCVARS%" >nul

echo Embedding bootstrap templates and Crinkler cores...
>"%TEMPBUILD%\embedded.rc" echo 101 RCDATA "1KB-gui-template.exe"
>>"%TEMPBUILD%\embedded.rc" echo 103 RCDATA "1KB-console-template.exe"
>>"%TEMPBUILD%\embedded.rc" echo 104 RCDATA "1KB-gui-crinkler.exe"
>>"%TEMPBUILD%\embedded.rc" echo 105 RCDATA "1KB-console-crinkler.exe"
>>"%TEMPBUILD%\embedded.rc" echo 106 RCDATA "1KB-gui-icon-core.exe"
>>"%TEMPBUILD%\embedded.rc" echo 107 RCDATA "1KB-console-icon-core.exe"
pushd "%TEMPBUILD%"
rc.exe /nologo /fo embedded.res embedded.rc
if errorlevel 1 (popd & goto :failed)
popd

echo Compiling 1KB.exe icon resource...
>"%TEMPBUILD%\icon.rc" echo 1 ICON "../src/1KB-logo.ico"
pushd "%TEMPBUILD%"
rc.exe /nologo /fo icon.res icon.rc
if errorlevel 1 (popd & goto :failed)
popd

echo Compiling 1KB.exe builder...
for %%F in (adler32 compress crc32 deflate trees zutil) do cl.exe /c third_party\zlib\%%F.c %COMMON% /wd4127 /wd4242 /wd4244 /wd4267 /wd4702 /Fo"%TEMPBUILD%\zlib-%%F.obj" || goto :failed
cl.exe /c src\icon-png-optimizer.cpp %COMMON% /DONEKB_BUILDER_ONLY /Fo"%TEMPBUILD%\icon-png-optimizer.obj" || goto :failed
cl.exe /c src\overlay-identity.cpp %COMMON% /DONEKB_BUILDER_ONLY /Fo"%TEMPBUILD%\overlay-identity-builder.obj" || goto :failed
cl.exe /c src\icon-crinkler-packer.cpp %COMMON% /DONEKB_BUILDER_ONLY /Fo"%TEMPBUILD%\icon-crinkler-packer.obj" || goto :failed
cl.exe /c src\deployment-manager.cpp %COMMON% /DONEKB_BUILDER_ONLY /Fo"%TEMPBUILD%\deployment-manager.obj" || goto :failed
cl.exe /c src\payload-crypto.cpp %COMMON% /DONEKB_BUILDER_ONLY /Fo"%TEMPBUILD%\payload-crypto-builder.obj" || goto :failed
cl.exe src\main.cpp "%TEMPBUILD%\icon-png-optimizer.obj" "%TEMPBUILD%\zlib-adler32.obj" "%TEMPBUILD%\zlib-compress.obj" "%TEMPBUILD%\zlib-crc32.obj" "%TEMPBUILD%\zlib-deflate.obj" "%TEMPBUILD%\zlib-trees.obj" "%TEMPBUILD%\zlib-zutil.obj" "%TEMPBUILD%\overlay-identity-builder.obj" "%TEMPBUILD%\deployment-manager.obj" "%TEMPBUILD%\payload-crypto-builder.obj" "%TEMPBUILD%\icon-crinkler-packer.obj" "%TEMPBUILD%\embedded.res" "%TEMPBUILD%\icon.res" %COMMON% /DONEKB_BUILDER_ONLY /Fo"%TEMPBUILD%\builder.obj" /Fd"%TEMPBUILD%\builder.pdb" /Fe"%TEMPBUILD%\1KB.exe" /link %LINKCOMMON% /HIGHENTROPYVA /SUBSYSTEM:CONSOLE || goto :failed

echo Compressing binaries...
third_party\upx.exe --best --lzma --no-progress "%TEMPBUILD%\1KB.exe" || goto :failed
third_party\upx.exe --best --lzma --no-progress "%TEMPBUILD%\runtime.exe" || goto :failed

move /y "%TEMPBUILD%\1KB.exe" "%CD%\1KB.exe" >nul || goto :failed
move /y "%TEMPBUILD%\runtime.exe" "%CD%\r" >nul || goto :failed
copy /y "%CD%\r" "%TMP%\r" >nul 2>nul
if errorlevel 1 (
  powershell.exe -NoProfile -Command "$target=[IO.Path]::GetFullPath('%TMP%\r'); Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $target } | ForEach-Object { taskkill.exe /PID $_.ProcessId /T /F | Out-Null; if($LASTEXITCODE){throw 'Could not stop the active runtime process tree.'} }" || goto :failed
  copy /y "%CD%\r" "%TMP%\r" >nul || goto :failed
)
if defined KEEP_BOOTSTRAP_TEMPLATES (
  if not exist "%KEEP_BOOTSTRAP_TEMPLATES%" mkdir "%KEEP_BOOTSTRAP_TEMPLATES%" || goto :failed
  copy /y "%TEMPBUILD%\1KB-gui-template.exe" "%KEEP_BOOTSTRAP_TEMPLATES%\gui-template.exe" >nul || goto :failed
  copy /y "%TEMPBUILD%\1KB-console-template.exe" "%KEEP_BOOTSTRAP_TEMPLATES%\console-template.exe" >nul || goto :failed
  if exist "%TEMPBUILD%\1KB-gui-crinkler.exe" (
    copy /y "%TEMPBUILD%\1KB-gui-crinkler.exe" "%KEEP_BOOTSTRAP_TEMPLATES%\gui-crinkler.exe" >nul || goto :failed
    copy /y "%TEMPBUILD%\1KB-console-crinkler.exe" "%KEEP_BOOTSTRAP_TEMPLATES%\console-crinkler.exe" >nul || goto :failed
  )
  copy /y "%TEMPBUILD%\1KB-gui-icon-core.exe" "%KEEP_BOOTSTRAP_TEMPLATES%\gui-icon-core.exe" >nul || goto :failed
  copy /y "%TEMPBUILD%\1KB-console-icon-core.exe" "%KEEP_BOOTSTRAP_TEMPLATES%\console-icon-core.exe" >nul || goto :failed
)
if exist "%CD%\1KB-template.exe" del /q "%CD%\1KB-template.exe"
rmdir /s /q "%TEMPBUILD%"

echo Built builder: %CD%\1KB.exe
echo Built publishable runtime: %CD%\r
echo Installed runtime: %TMP%\r
popd
exit /b 0

:failed
if defined TEMPBUILD if exist "%TEMPBUILD%" rmdir /s /q "%TEMPBUILD%"
echo ERROR: 1KB.exe build failed. Review the error above.
popd
exit /b 1
