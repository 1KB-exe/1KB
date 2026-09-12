@echo off
setlocal
pushd "%~dp0.."
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
if not exist ignore\update-tests-x86 mkdir ignore\update-tests-x86
for %%F in (adler32 crc32 deflate trees zutil inflate inftrees inffast) do cl /nologo /c /O2 /MT third_party\zlib\%%F.c /Foignore\update-tests-x86\zlib-%%F.obj >nul || exit /b 1
cl /nologo /c /O2 /MT third_party\lzma\LzmaDec.c /Foignore\update-tests-x86\lzma.obj >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 tests\test_update_system.cpp ignore\update-tests-x86\zlib-*.obj ignore\update-tests-x86\lzma.obj /Foignore\update-tests-x86\test.obj /Feignore\update-tests-x86\test.exe /link bcrypt.lib || exit /b 1
ignore\update-tests-x86\test.exe || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 tests\large_crypto_driver.cpp src\payload-crypto.cpp /Foignore\update-tests-x86\ /Feignore\update-tests-x86\crypto.exe /link bcrypt.lib || exit /b 1
py -3 tests\test_large_packages.py ignore\update-tests-x86\test.exe ignore\update-tests-x86\crypto.exe
exit /b %errorlevel%
