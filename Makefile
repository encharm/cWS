CPP_SHARED := -DUSE_LIBUV -g -O3 -I ./src -shared -fPIC ./src/Extensions.cpp ./src/Group.cpp ./src/Networking.cpp ./src/Hub.cpp ./src/cSNode.cpp ./src/WebSocket.cpp ./src/HTTPSocket.cpp ./src/Socket.cpp ./src/Epoll.cpp ./src/Addon.cpp ./src/Zlib.cpp -Wno-deprecated-declarations -Wno-unused-result -fvisibility=hidden -DNODE_WANT_INTERNALS
CPP_LINUX := -std=c++20
CPP_OSX := -std=c++20 -stdlib=libc++ -mmacosx-version-min=10.15 -undefined dynamic_lookup

VER_115 := v20.10.0
VER_127 := v22.12.0
VER_137 := v24.7.0
VER_147 := v26.8.1

ARCH := $(shell node -p process.arch)
OS := $(shell uname -s)

# zlib-ng (vendored in deps/zlib-ng) is built once per OS/arch with CMake and linked
# statically into every binding; the addon uses its native zng_ API (-DCWS_ZLIB_NG).
ZLIBNG := deps/zlib-ng
ZLIBNG_BUILD := $(ZLIBNG)/build-$(OS)-$(ARCH)
ZLIBNG_LIB := $(ZLIBNG_BUILD)/libz-ng.a
ZLIBNG_CMAKE := -DZLIB_COMPAT=OFF -DZLIB_ENABLE_TESTS=OFF -DZLIBNG_ENABLE_TESTS=OFF -DWITH_GTEST=OFF -DWITH_GZFILEOP=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=Release
ifeq ($(OS),Darwin)
ZLIBNG_CMAKE += -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15
endif
CPP_ZLIBNG := -DCWS_ZLIB_NG -I $(ZLIBNG_BUILD)

default:
	make targets
	V=20 NODE=targets/node-$(VER_115) ABI=115 make `(uname -s)`
	V=22 NODE=targets/node-$(VER_127) ABI=127 make `(uname -s)`
	V=24 NODE=targets/node-$(VER_137) ABI=137 make `(uname -s)`
	V=26 NODE=targets/node-$(VER_147) ABI=147 make `(uname -s)`
	for f in dist/bindings/*.node; do chmod +x $$f; done
targets:
	mkdir -p targets
	curl https://nodejs.org/dist/$(VER_115)/node-$(VER_115)-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/$(VER_127)/node-$(VER_127)-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/$(VER_137)/node-$(VER_137)-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/$(VER_147)/node-$(VER_147)-headers.tar.gz | tar xz -C targets
$(ZLIBNG_LIB):
	cmake -S $(ZLIBNG) -B $(ZLIBNG_BUILD) $(ZLIBNG_CMAKE)
	cmake --build $(ZLIBNG_BUILD) --parallel

zlibng: $(ZLIBNG_LIB)

Linux: $(ZLIBNG_LIB)
	g++ $(CPP_SHARED) $(CPP_ZLIBNG) $(CPP_LINUX) -I $$NODE/include/node -I $$NODE/src -I $$NODE/deps/uv/include -I $$NODE/deps/v8/include -I $$NODE/deps/openssl/openssl/include -I $$NODE/deps/zlib -I src/headers/$$V -s -o dist/bindings/cws_linux_$(ARCH)_node$(ABI).node -DHAVE_OPENSSL=1 $(ZLIBNG_LIB)
Darwin: $(ZLIBNG_LIB)
	g++ $(CPP_SHARED) $(CPP_ZLIBNG) $(CPP_OSX) -I $$NODE/include/node -I src/headers/$$V -o dist/bindings/cws_darwin_$(ARCH)_node$(ABI).node -DHAVE_OPENSSL=1 $(ZLIBNG_LIB)
