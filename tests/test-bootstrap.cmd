@echo off
setlocal
pushd "%~dp0.."
set "T=%CD%\.1KB-test"
if exist "%T%" rmdir /s /q "%T%"
call build-1KB.cmd || goto :fail
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
mkdir "%T%" || goto :fail
>"%T%\fixture-icon.rc" echo 1 ICON "../src/1KB-logo.ico"
pushd "%T%"
rc.exe /nologo /fo fixture-icon.res fixture-icon.rc || (popd & goto :fail)
popd
cl.exe tests\capture-runtime.c "%T%\fixture-icon.res" /nologo /O1 /Os /GS- /Zl /Fo"%T%\capture.obj" /Fe"%T%\capture-runtime.exe" /link kernel32.lib /nodefaultlib /entry:Entry /subsystem:console /fixed /opt:ref || goto :fail
cl.exe tests\capture-runtime.c "%T%\fixture-icon.res" /nologo /O1 /Os /GS- /Zl /Fo"%T%\capture-gui.obj" /Fe"%T%\capture-gui.exe" /link kernel32.lib /nodefaultlib /entry:Entry /subsystem:windows /fixed /opt:ref || goto :fail
cl.exe tests\capture-runtime.c /nologo /O1 /Os /GS- /Zl /Fo"%T%\capture-no-icon.obj" /Fe"%T%\capture-no-icon.exe" /link kernel32.lib /nodefaultlib /entry:Entry /subsystem:console /fixed /opt:ref || goto :fail
call "%VSROOT%\VC\Auxiliary\Build\vcvars32.bat" >nul
cl.exe /c src\overlay-identity.cpp /nologo /std:c++17 /permissive- /O1 /Os /EHsc /DUNICODE /D_UNICODE /Fo"%T%\overlay-identity.obj" || goto :fail
cl.exe tests\test_overlay_identity.cpp "%T%\overlay-identity.obj" /nologo /std:c++17 /permissive- /O1 /Os /EHsc /DUNICODE /D_UNICODE /Fo"%T%\overlay-identity-test.obj" /Fe"%T%\overlay-identity-test.exe" || goto :fail
"%T%\overlay-identity-test.exe" || goto :fail
cl.exe /c src\payload-crypto.cpp /nologo /std:c++17 /permissive- /O1 /Os /EHsc /DUNICODE /D_UNICODE /Fo"%T%\payload-crypto.obj" || goto :fail
cl.exe tests\test_payload_crypto.cpp "%T%\payload-crypto.obj" /nologo /std:c++17 /permissive- /O1 /Os /EHsc /DUNICODE /D_UNICODE /Fo"%T%\payload-test.obj" /Fe"%T%\payload-crypto-test.exe" /link bcrypt.lib || goto :fail
"%T%\payload-crypto-test.exe" || goto :fail
cl.exe src\main.cpp "%T%\overlay-identity.obj" "%T%\payload-crypto.obj" /nologo /std:c++17 /permissive- /O1 /Os /EHsc /DUNICODE /D_UNICODE /DONEKB_RUNTIME_ONLY /DONEKB_RUNTIME_TESTS /Fo"%T%\runtime-test.obj" /Fe"%T%\runtime-test.exe" /link winhttp.lib shell32.lib ole32.lib user32.lib gdi32.lib bcrypt.lib /SUBSYSTEM:WINDOWS || goto :fail
py -3 tests\test_bootstrap.py --builder "%CD%\1KB.exe" --capture-runtime "%T%\capture-runtime.exe" --gui-runtime "%T%\capture-gui.exe" --no-icon-runtime "%T%\capture-no-icon.exe" --test-runtime "%T%\runtime-test.exe" || goto :fail
echo All launcher and runtime tests passed.
set "TEST_EXIT=0"
goto :cleanup
:fail
echo Bootstrap tests failed.
set "TEST_EXIT=1"
:cleanup
if exist "%T%" rmdir /s /q "%T%"
popd
exit /b %TEST_EXIT%
