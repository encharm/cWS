# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`@encharm/cws` is a native C++ WebSocket implementation for Node.js that aims to be a near drop-in replacement for the `ws` module. It is a fork of ClusterWS/cWS, which itself forked uWebSockets v0.14 (dual MIT + ZLIB license, see `LICENSE` and `src/LICENSE`). Prebuilt binaries for every supported platform/ABI are committed under `dist/bindings/` and published with the package; end users never compile.

## Commands

```sh
npm run build-ts        # lint + tsc: lib/*.ts -> dist/*.js + *.d.ts (dist JS is committed)
npm run lint            # tslint over lib/**/*.ts
npm test                # nyc + mocha, runs tests/**/*.test.ts via ts-node (loads ../lib, not dist)
npx mocha -r ts-node/register tests/server-client.test.ts -g "ping/pong" --exit   # single test
npm run build-cpp       # node-gyp rebuild for the *current* Node ABI only; output goes to build_log.txt, never fails the install
npm run clean           # deletes dist/bindings/* (all committed binaries!) -- don't run casually
```

Tests bind ports 3000 (ws) and 3001 (wss, certs in `tests/certs/`). The test file currently iterates only `['Non-SSL']`; the SSL loop is wired up but disabled. `tests/autobahn-*.js` are manual Autobahn testsuite harnesses, not part of `npm test`.

### Building the release binaries (multi-ABI)

`binding.gyp` is only used by `npm install` for a local build against the running Node (Node's zlib, no zlib-ng), and only when `scripts/install.js` finds no prebuilt binding for the platform/ABI (`CWS_FORCE_BUILD=1` overrides); it must carry the same include path and defines as the Makefile or it fails silently (the install script discards the exit code). Release binaries are built by the `Makefile` (macOS/Linux) and `make.bat` (Windows), which:

1. Download official Node header tarballs for one pinned version per supported major into `targets/` (`VER_115`=Node 20, `VER_127`=Node 22, `VER_137`=Node 24, `VER_147`=Node 26; the number is the Node ABI / `process.versions.modules`).
2. Compile `src/*.cpp` once per ABI with `g++`/`cl` directly, with `-I src/headers/$V` for the matching Node major.
3. Build the vendored zlib-ng (`deps/zlib-ng`, native `zng_` API) once per OS/arch: CMake into `deps/zlib-ng/build-<OS>-<arch>/` on macOS/Linux, `nmake -f win32\Makefile.msc` on Windows. The bindings are compiled with `-DCWS_ZLIB_NG` and link it statically. `src/Zlib.cpp` is the only file that includes a zlib header; without the define (node-gyp fallback) it uses Node's zlib.
4. Emit `dist/bindings/cws_<platform>_<arch>_node<ABI>.node`.

```sh
make                                  # all four ABIs for the host OS/arch (needs curl, cmake, g++/clang)
./build-on-docker.sh linux/arm64      # Linux arm64 (native on Apple Silicon, ~1 min)
./build-on-docker.sh linux/amd64      # Linux x64 (default; emulated on Apple Silicon, slower)
make.bat                              # Windows x64; auto-detects VS 2022 (Build Tools or any edition)
```

All of it can be driven from the Mac. Docker builds mount the repo and write straight into `dist/bindings/`. The Windows build runs in the Parallels "Windows 11" VM through the shared folder (cmd refuses UNC batch paths, hence `pushd`):

```sh
prlctl resume "Windows 11"
prlctl exec "Windows 11" cmd /c "pushd \\\\Mac\\code\\cWS && make.bat"   # 4 backslashes in zsh -> \\Mac
prlctl suspend "Windows 11"
```

The VM is Windows on ARM: `vcvars64` still emits x64 code, so `make.bat` names outputs `win32_x64` explicitly, and testing them needs an x64 Node in the guest (an x64 zip from nodejs.org extracted under `C:\Temp` works; the guest's own ARM64 Node cannot load them). Docker base image is Ubuntu 22.04 = the minimum glibc baseline. Smoke-test a binding on its target Node with `node node_modules/mocha/bin/mocha -r ts-node/register 'tests/**/*.test.ts' --exit`, e.g. inside `node:22-bookworm` for Linux.

Adding a new Node major means: add a `VER_xxx`/ABI line to `Makefile` and `make.bat`, vendor that Node version's private headers into `src/headers/<major>/`, and add a `#if NODE_MAJOR_VERSION==<major>` include block in `src/Addon.h`. Vendoring = copy every `src/**/*.h` from the Node source tarball, plus `deps/ncrypto/ncrypto.h` and any `deps/v8/include/*.h` missing from the official headers tarball, then make `ssl_` public in `crypto/crypto_tls.h` (`TLSWrapSSLGetter` needs it). For 26 there is a second edit: `MaybeStackBuffer::AllocateSufficientStorage` (primary and the concept-constrained `V8Type` partial specialization) and `ToPath` are defined in-class in `util.h` instead of out-of-line in `util-inl.h`, because MSVC `cl.exe` cannot match out-of-line member definitions once that constrained specialization exists (Node builds Windows with clang-cl, which the Parallels VM does not have). Both edits are marked with a `cWS:` comment. Node's gyp-only config macros (e.g. `HAVE_SQLITE`, `HAVE_AMARO` for 26) must be defined before the include. Expect V8 API churn in `Addon.h` on each major; 26 needed tagged internal-field reads (`BaseObject::FromJSObject`) and dropped `Object::GetIsolate` and the `MakeCallback` overload without `async_context`.

## Architecture

### Two layers, one binary

- **`lib/*.ts` (thin JS facade)** compiled to `dist/`. `shared.ts` loads the native addon by filename `cws_${platform}_${arch}_node${versions.modules}` and wires native group callbacks (`onConnection`, `onMessage`, `onDisconnection`, …) to per-socket `registeredEvents`. Events are single-listener: `on()` overwrites, with a console warning. `client.ts` exports `WebSocket` (used for both client sockets and server-side peers, distinguished by `options.external`), `server.ts` exports `WebSocketServer` (aliased as `WebSocket.Server`).
- **`src/` (C++ addon)** is the uWebSockets 0.14 core (`Hub`, `Group`, `WebSocket`, `HTTPSocket`, `Networking`, `WebSocketProtocol.h`, `Extensions` for permessage-deflate) plus the V8 binding layer in `src/Addon.h` / `Addon.cpp`. `Backend.h` picks the event backend: libuv everywhere (`-DUSE_LIBUV` is always set), the epoll backend in `Epoll.*` is legacy and not compiled in by the Makefile.

### How a server connection gets into C++ (the important part)

cWS does **not** run its own HTTP listener. `WebSocketServer` uses a Node `http`/`https` server (user-supplied or created internally) and hooks its `'upgrade'` event. In `upgradeConnection()`:

1. It grabs the raw libuv handle from the Node socket (`socket._handle`, or `_parent._handle` for TLS).
2. For TLS it calls `native.getSSLContext(socket.ssl)` to pull the raw `SSL*` out of Node's internal `TLSWrap`. This is why `src/headers/<major>/` vendors Node **private** headers (`crypto/crypto_tls.h`, `base_object-inl.h`, etc.) and `Addon.h` defines `NODE_WANT_INTERNALS` around them: `TLSWrapSSLGetter` subclasses `node::crypto::TLSWrap` to reach the protected `ssl_` member. This is the ABI-fragile part and the reason binaries are per-Node-major.
3. `native.transfer(fd, ssl)` creates a `Ticket` and the Node socket is destroyed. On its `'close'` event, `native.upgrade(group, ticket, secKey, extensions, protocol)` hands the fd to the uWS `Hub`, which performs the WebSocket handshake and owns the socket from then on. On Linux with Node >= 20 the fd is `dup()`ed first as a workaround for io_uring (see comment in `Addon.h`).

`noServer: true` + `handleUpgrade()` follows the same path for `ws` compatibility. `path` and `verifyClient` checks happen in JS before the transfer.

### Client connections

The client side never touches Node sockets: `native.connect(clientGroup, url, ws)` lets the uWS `Hub` do TCP/TLS itself using a process-wide `SSL_CTX` created in `cSNode.cpp` (TLS 1.0–1.2 only). `secureProtocol = 'TLSv1_2_method'` is exported for users to pin their https server to match.

### Write corking

`Socket::sendTransformed` does not write when the socket is corkable (WebSockets only, plain TCP only) and `NodeData::corkState->enabled` is set. It frames the message into the socket's `messageQueue` and registers the socket in the per-loop `corkState->pending` list. `Socket::flushCorked` drains that list with one `sendmsg`/`WSASend` gather per socket from two libuv hooks registered in `registerCheck` (`src/Addon.h`): a `uv_prepare` (before the loop blocks in poll) and the existing `uv_check` (after poll and after Node's immediates). Anything that cannot be written falls through to the normal `UV_WRITABLE` drain loop in `ioHandler`. `closeSocket`/`onEnd`/`transfer` call `flushCorkedOnClose`, which unregisters the socket and pushes what it can to the kernel without invoking callbacks. `CWS_CORK=0` disables it at addon load (`corkEnabledFromEnv`). `HttpSocket` is never corkable because `Hub::upgrade` deletes it synchronously.

### Send worker thread

`SendWorker` (`src/SendWorker.cpp`) starts one `std::thread` at addon load when corking is enabled. `Socket::uncork` moves up to 512 queued frames into a `Socket::SendOp` (ownership moves with them) and hands it over through `deps/readerwriterqueue` (blocking SPSC main→worker, plain SPSC worker→main plus a `uv_async`). The worker only calls `sendmsg`/`WSASend` and fills `result`/`error`; `Socket::sendComplete` on the main thread pops what was sent, requeues the rest at the head, resubmits if more queued, arms `UV_WRITABLE` for the classic drain loop on a short write, runs send callbacks, and calls `endCb` (= `STATE::onEnd`, set in `setState`) on a hard error. While an op is in flight `write()` appends to the queue and the drain loop stays out. `closeSocket` orphans an in-flight op (`socket = nullptr`, `closeFd = true`) and lets the completion close the fd, so a reused fd number can never receive the old socket's bytes. SSL sockets never use the worker. `CWS_SEND_THREAD=0` disables.

### Things that are easy to get wrong

- `dist/*.js` and `dist/bindings/*.node` are committed. Rebuild TS with `npm run build-ts` and commit the output; binaries must be rebuilt on each platform when C++ changes.
- Binary messages reach JS as an `ArrayBuffer` that is a zero-copy view over the per-loop receive buffer shared by every socket. `onMessage` in `Addon.h` detaches it after the handler returns, so retaining it without `slice()` throws on later use rather than exposing another connection's bytes. Text frames are copied into a V8 string. Ping/pong payloads also go through the string path.
- Every handler, including `message`, is invoked through `node::MakeCallback`, which establishes an async-context scope and drains nextTick/microtasks after the handler returns. Because that drain happens before `onMessage` detaches the binary `ArrayBuffer`, a continuation that resolves within it still sees the bytes; anything that waits on real I/O sees a detached buffer.
- `shared.ts` currently has a leftover `console.log('log', ...)` printing the binding path on load.
- The addon exposes one `Group` per server plus a single shared client group; `ws.external` is the C++ `WebSocket*` and is nulled on disconnect, so every native call in `client.ts` guards on it.
- permessage-deflate: `Group` carries `deflateWindowBits`/`deflateMemLevel` (tier of the per-socket compressor allocated in the `WebSocket` ctor); the negotiator advertises `server_max_window_bits` when below 15 but ignores a smaller value requested by the client. The JS `send` decides `compress` per message: explicit option, else `compressThreshold` from the server config (undefined when deflate is off). Client-to-server inflate is one shared stream reset per message (`client_no_context_takeover` is always offered).
- Close code defaults to 1005 when the peer sends none (matches `ws`). App-level ping uses the single byte `'9'` (`APP_PING_CODE`) for browser clients that can't see protocol pings.
