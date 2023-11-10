REM Fix this path !!!
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" x64

set v83=v14.5.0
set v93=v16.11.1
set v108=v18.3.0
set v115=v20.1.0

for /f %%i in ('node -p process.arch') do set ARCH=%%i

if not exist targets (
  mkdir targets
  curl https://nodejs.org/dist/%v83%/node-%v83%-headers.tar.gz | tar xz -C targets
  curl https://nodejs.org/dist/%v83%/win-x64/node.lib > targets/node-%v83%/node.lib
  curl https://nodejs.org/dist/%v93%/node-%v93%-headers.tar.gz | tar xz -C targets
  curl https://nodejs.org/dist/%v93%/win-x64/node.lib > targets/node-%v93%/node.lib
  curl https://nodejs.org/dist/%v108%/node-%v108%-headers.tar.gz | tar xz -C targets
  curl https://nodejs.org/dist/%v108%/win-x64/node.lib > targets/node-%v108%/node.lib
  curl https://nodejs.org/dist/%v115%/node-%v115%-headers.tar.gz | tar xz -C targets
  curl https://nodejs.org/dist/%v115%/win-x64/node.lib > targets/node-%v115%/node.lib
)

cl /I targets/node-%v83%/include/node /I targets/node-%v83%/deps/uv/include /I targets/node-%v83%/deps/v8/include /I targets/node-%v83%/deps/openssl/openssl/include /I targets/node-%v83%/deps/zlib /I src/headers/14 /EHsc /Ox /LD /Fedist/bindings/cws_win32_%ARCH%_node83.node src/*.cpp targets/node-%v83%/node.lib
cl /I targets/node-%v93%/include/node /I targets/node-%v93%/deps/uv/include /I targets/node-%v93%/deps/v8/include /I targets/node-%v93%/deps/openssl/openssl/include /I targets/node-%v93%/deps/zlib /I src/headers/16 /EHsc /Ox /LD /Fedist/bindings/cws_win32_%ARCH%_node93.node src/*.cpp targets/node-%v93%/node.lib
cl /std:c++17 /I targets/node-%v108%/include/node /I targets/node-%v108%/deps/uv/include /I targets/node-%v108%/deps/v8/include /I targets/node-%v108%/deps/openssl/openssl/include /I targets/node-%v108%/deps/zlib /I src/headers/18 /EHsc /Ox /LD /Fedist/bindings/cws_win32_%ARCH%_node108.node src/*.cpp targets/node-%v108%/node.lib
cl /std:c++17 /I targets/node-%v115%/include/node /I targets/node-%v115%/deps/uv/include /I targets/node-%v115%/deps/v8/include /I targets/node-%v115%/deps/openssl/openssl/include /I targets/node-%v115%/deps/zlib /I src/headers/20 /EHsc /Ox /LD /Fedist/bindings/cws_win32_%ARCH%_node115.node src/*.cpp targets/node-%v115%/node.lib

del ".\*.obj"
del ".\dist\bindings\*.exp"
del ".\dist\bindings\*.lib"