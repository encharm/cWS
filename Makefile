CPP_SHARED := -DUSE_LIBUV -std=c++20 -g -O3 -I ./src -shared -fPIC ./src/Extensions.cpp ./src/Group.cpp ./src/Networking.cpp ./src/Hub.cpp ./src/cSNode.cpp ./src/WebSocket.cpp ./src/HTTPSocket.cpp ./src/Socket.cpp ./src/Epoll.cpp ./src/Addon.cpp -Wno-deprecated-declarations -Wno-unused-result -fvisibility=hidden -DNODE_WANT_INTERNALS
CPP_OSX := -stdlib=libc++ -mmacosx-version-min=10.15 -undefined dynamic_lookup

VER_115 := v20.10.0
VER_127 := v22.12.0

ARCH := `(node -p process.arch)`

default:
	make targets
	V=20 NODE=targets/node-$(VER_115) ABI=115 make `(uname -s)`
	V=22 NODE=targets/node-$(VER_127) ABI=127 make `(uname -s)`
	for f in dist/bindings/*.node; do chmod +x $$f; done
targets: 
	mkdir -p targets
	curl https://nodejs.org/dist/$(VER_115)/node-$(VER_115)-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/$(VER_127)/node-$(VER_127)-headers.tar.gz | tar xz -C targets
Linux:
	g++ $(CPP_SHARED) -I $$NODE/include/node -I $$NODE/src -I $$NODE/deps/uv/include -I $$NODE/deps/v8/include -I $$NODE/deps/openssl/openssl/include -I $$NODE/deps/zlib -I src/headers/$$V -s -o dist/bindings/cws_linux_$(ARCH)_node$(ABI).node -DHAVE_OPENSSL=1
Darwin:
	g++ $(CPP_SHARED) $(CPP_OSX) -I $$NODE/include/node -I src/headers/$$V -o dist/bindings/cws_darwin_$(ARCH)_node$(ABI).node -DHAVE_OPENSSL=1
