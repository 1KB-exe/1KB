@echo off
setlocal
pushd "%~dp0.."
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist ignore\update-tests mkdir ignore\update-tests
for %%F in (adler32 crc32 deflate trees zutil inflate inftrees inffast) do cl /nologo /c /O2 /MT third_party\zlib\%%F.c /Foignore\update-tests\zlib-%%F.obj >nul || exit /b 1
cl /nologo /c /O2 /MT third_party\lzma\LzmaDec.c /Foignore\update-tests\lzma.obj >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /MT /utf-8 tests\test_update_system.cpp ignore\update-tests\zlib-*.obj ignore\update-tests\lzma.obj /Foignore\update-tests\test.obj /Feignore\update-tests\test.exe /link bcrypt.lib || exit /b 1
ignore\update-tests\test.exe || exit /b 1
py -3 tests\test_zip_security.py ignore\update-tests\test.exe || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /MT /utf-8 tests\test_payload_crypto.cpp src\payload-crypto.cpp ignore\update-tests\zlib-*.obj ignore\update-tests\lzma.obj /Foignore\update-tests\ /Feignore\update-tests\crypto.exe /link bcrypt.lib || exit /b 1
ignore\update-tests\crypto.exe || exit /b 1
cl /nologo /c /std:c++17 /EHsc /W4 /wd4505 /MT /utf-8 /DUNICODE /D_UNICODE /DONEKB_RUNTIME_ONLY src\main.cpp /Foignore\update-tests\runtime.obj || exit /b 1
cl /nologo /c /std:c++17 /EHsc /W4 /wd4505 /MT /utf-8 /DUNICODE /D_UNICODE /DONEKB_BUILDER_ONLY src\deployment-manager.cpp /Foignore\update-tests\manager.obj || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE tests\update_runtime_driver.cpp src\overlay-identity.cpp src\payload-crypto.cpp ignore\update-tests\zlib-*.obj ignore\update-tests\lzma.obj /Foignore\update-tests\ /Feignore\update-tests\runtime-driver.exe /link winhttp.lib shell32.lib ole32.lib user32.lib gdi32.lib bcrypt.lib /SUBSYSTEM:CONSOLE || exit /b 1
py -3 tests\test_update_runtime.py ignore\update-tests\runtime-driver.exe || exit /b 1
exit /b 0
