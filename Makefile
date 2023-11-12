CPP_SHARED := -DUSE_LIBUV -std=gnu++17 -Og -g -I ./src -shared -fPIC ./src/Extensions.cpp ./src/Group.cpp ./src/Networking.cpp ./src/Hub.cpp ./src/cSNode.cpp ./src/WebSocket.cpp ./src/HTTPSocket.cpp ./src/Socket.cpp ./src/Epoll.cpp ./src/Addon.cpp -Wno-deprecated-declarations -Wno-unused-result
CPP_OSX := -stdlib=libc++ -mmacosx-version-min=10.7 -undefined dynamic_lookup

VER_83 := v14.5.0
VER_93 := v16.11.1
VER_108 := v18.3.0
VER_115 := v20.1.0

ARCH := `(uname -m | sed 's/86_//')`

default:
	make targets
	V=14 NODE=targets/node-$(VER_83) ABI=83 make `(uname -s)`
	V=16 NODE=targets/node-$(VER_93) ABI=93 make `(uname -s)`
	V=18 NODE=targets/node-$(VER_108) ABI=108 make `(uname -s)`
	V=20 NODE=targets/node-$(VER_115) ABI=115 make `(uname -s)`
	for f in dist/bindings/*.node; do chmod +x $$f; done
targets: 
	mkdir -p targets
	curl https://nodejs.org/dist/$(VER_83)/node-$(VER_83)-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/$(VER_93)/node-$(VER_93)-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/$(VER_108)/node-$(VER_108)-headers.tar.gz | tar xz -C targets
	curl https://nodejs.org/dist/$(VER_115)/node-$(VER_115)-headers.tar.gz | tar xz -C targets
Linux:
	g++ $(CPP_SHARED) -static-libstdc++ -static-libgcc -I $$NODE/include/node -I $$NODE/src -I $$NODE/deps/uv/include -I $$NODE/deps/v8/include -I $$NODE/deps/openssl/openssl/include -I $$NODE/deps/zlib -I src/headers/$$V -s -o dist/bindings/cws_linux_$(ARCH)_node$(ABI).node -DHAVE_OPENSSL=1
Darwin:
	g++ $(CPP_SHARED) $(CPP_OSX) -I $$NODE/include/node -I src/headers/$$V -o dist/bindings/cws_darwin_$(ARCH)_node$(ABI).node -DHAVE_OPENSSL=1
