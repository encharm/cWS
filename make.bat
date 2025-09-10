REM Fix this path !!!
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
@REM call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" x64

set v115=v20.9.0
set v127=v22.12.0
set v137=v24.7.0

for /f %%i in ('node -p process.arch') do set ARCH=%%i

if not exist targets (
  mkdir targets
  curl https://nodejs.org/dist/%v115%/node-%v115%-headers.tar.gz | tar xz -C targets
  curl https://nodejs.org/dist/%v115%/win-x64/node.lib > targets/node-%v115%/node.lib
  curl https://nodejs.org/dist/%v127%/node-%v127%-headers.tar.gz | tar xz -C targets
  curl https://nodejs.org/dist/%v127%/win-x64/node.lib > targets/node-%v127%/node.lib
  curl https://nodejs.org/dist/%v137%/node-%v137%-headers.tar.gz | tar xz -C targets
  curl https://nodejs.org/dist/%v137%/win-x64/node.lib > targets/node-%v137%/node.lib
)

cl /std:c++17 /I targets/node-%v115%/include/node /I targets/node-%v115%/deps/uv/include /I targets/node-%v115%/deps/v8/include /I targets/node-%v115%/deps/openssl/openssl/include /I targets/node-%v115%/deps/zlib /I src/headers/20 /EHsc /Ox /LD /Fedist/bindings/cws_win32_%ARCH%_node115.node src/*.cpp targets/node-%v115%/node.lib
cl /std:c++20 /I targets/node-%v127%/include/node /I targets/node-%v127%/deps/uv/include /I targets/node-%v127%/deps/v8/include /I targets/node-%v127%/deps/openssl/openssl/include /I targets/node-%v127%/deps/zlib /I src/headers/22 /EHsc /Ox /LD /Fedist/bindings/cws_win32_%ARCH%_node127.node src/*.cpp targets/node-%v127%/node.lib
cl /std:c++20 /I targets/node-%v137%/include/node /I targets/node-%v137%/deps/uv/include /I targets/node-%v137%/deps/v8/include /I targets/node-%v137%/deps/openssl/openssl/include /I targets/node-%v137%/deps/zlib /I src/headers/23 /EHsc /Ox /LD /Fedist/bindings/cws_win32_%ARCH%_node137.node src/*.cpp targets/node-%v137%/node.lib

del ".\*.obj"
del ".\dist\bindings\*.exp"
del ".\dist\bindings\*.lib"
