@echo off
REM Builds dist\bindings\cws_win32_<arch>_node<ABI>.node for Node 20, 22 and 24.
REM Needs Visual Studio 2022 (Build Tools or any edition) with the C++ workload, plus node, curl and tar on PATH.
REM Run from the repository root (a `pushd \\server\share\cWS` mapped drive works too).

set VCVARS=
for %%p in (
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%~p set "VCVARS=%%~p"
if not defined VCVARS (
  echo Could not find vcvars64.bat for Visual Studio 2022. Edit make.bat.
  exit /b 1
)
call "%VCVARS%" x64 || exit /b 1

REM Keep these in sync with the Makefile (same header tarballs are reused when targets\ already exists).
set v115=v20.10.0
set v127=v22.12.0
set v137=v24.7.0
set v147=v26.8.1

REM vcvars64 targets x64 regardless of the host (e.g. Windows on ARM under Parallels), so name the output by the target, not by node's arch.
set ARCH=x64

if not exist targets mkdir targets
for %%v in (%v115% %v127% %v137% %v147%) do (
  if not exist targets\node-%%v\include (
    echo Downloading headers for %%v
    curl -sL https://nodejs.org/dist/%%v/node-%%v-headers.tar.gz | tar xz -C targets || exit /b 1
  )
  if not exist targets\node-%%v\node.lib (
    echo Downloading node.lib for %%v
    curl -sL https://nodejs.org/dist/%%v/win-x64/node.lib -o targets\node-%%v\node.lib || exit /b 1
  )
)

REM zlib-ng (vendored) built once as a static lib with its NMake makefile; -MT matches the addon's CRT.
REM zconf-ng.h is generated from zconf-ng.h.in by the NMake step; a CMake run of zlib-ng (the
REM Linux/macOS builds) renames it away, so regenerate it whenever it is missing.
if not exist deps\zlib-ng\zconf-ng.h copy /y deps\zlib-ng\zconf-ng.h.in deps\zlib-ng\zconf-ng.h >nul
if not exist deps\zlib-ng\zlib-ng.lib (
  pushd deps\zlib-ng
  nmake -nologo -f win32\Makefile.msc LOC="-MT" zlib-ng.lib || (popd & exit /b 1)
  popd
)

set CLFLAGS=/nologo /std:c++20 /Zc:__cplusplus /EHsc /Ox /LD /DUSE_LIBUV /DHAVE_OPENSSL=1 /DCWS_ZLIB_NG /I deps\zlib-ng /I deps\readerwriterqueue
set SOURCES=src\Addon.cpp src\Extensions.cpp src\Group.cpp src\Networking.cpp src\Hub.cpp src\cSNode.cpp src\WebSocket.cpp src\HTTPSocket.cpp src\Socket.cpp src\Zlib.cpp src\SendWorker.cpp

call :build 20 %v115% 115 || exit /b 1
call :build 22 %v127% 127 || exit /b 1
call :build 24 %v137% 137 || exit /b 1
call :build 26 %v147% 147 || exit /b 1

del /q .\*.obj 2>nul
del /q .\dist\bindings\*.exp 2>nul
del /q .\dist\bindings\*.lib 2>nul
echo Done.
exit /b 0

:build
set MAJOR=%~1
set NODEVER=%~2
set ABI=%~3
set T=targets\node-%NODEVER%
echo === Node %MAJOR% (%NODEVER%, ABI %ABI%)
cl %CLFLAGS% /I %T%\include\node /I %T%\deps\uv\include /I %T%\deps\v8\include /I %T%\deps\openssl\openssl\include /I %T%\deps\zlib /I src\headers\%MAJOR% /Fedist\bindings\cws_win32_%ARCH%_node%ABI%.node %SOURCES% %T%\node.lib deps\zlib-ng\zlib-ng.lib
exit /b %ERRORLEVEL%
