## Released 4.10.0
* microdeflate: a built-in ~150-line raw-DEFLATE encoder (fixed Huffman, greedy LZ77, no stream state, no per-message hash reset) now compresses independent messages, i.e. the shared-compressor mode. Measured on a real BSON RPC stream: same ratio as zlib-ng level 1 (2.84 vs 2.85) at ~1.7x its speed; a compressed 2 KB message costs 4.6 µs of worker CPU instead of 6.0. Output is standard DEFLATE (round-trip tested against zlib's inflater). `CWS_MICRO_DEFLATE=0` falls back to zlib-ng; `zlibBackend` reports `+ microdeflate` when active. Dedicated windows (context takeover) and inflate stay on zlib-ng.
* Compression moved to the send worker: `send()` of a compressed message queues the raw payload and the worker deflates + frames it. Main-thread cost of a compressed 2 KB RPC message drops from ~6 µs to ~0.7 µs (Linux, per-thread measurement); wire output is byte-identical. Main-thread write paths (same-tick terminate, full worker queue, drain loop after a short write) deflate pending messages themselves first.

## Released 4.9.0
* Send worker thread: the end-of-tick gathered writes run on a dedicated thread (lock-free SPSC hand-off via the vendored `readerwriterqueue`), taking the kernel's TCP work off the JavaScript thread. Measured on a loaded EPYC: 30-65% less main-thread CPU per message, 1.7-2.2x fan-out throughput. `CWS_SEND_THREAD=0` disables; `sendThread` export reports status. A socket closed with a send in flight keeps its fd open until that send completes (prevents fd reuse races).

## Released 4.8.4
* `npm install` no longer rebuilds the binding from source when a prebuilt one matches the platform and Node ABI. Since the node-gyp fallback was repaired in 4.8.0, every install on a machine with a compiler was silently replacing the shipped zlib-ng binding with a Node-zlib build. `CWS_FORCE_BUILD=1` forces the source build.

## Released 4.8.3
* Corked sends up to 1 KB use the per-loop block pool instead of malloc/free per message; queued messages remember their pool block. Also initialise the `reserved` callback argument, which was passed uninitialised for messages that hit the write queue.
* JS: resolve the native namespace once per socket instead of on every `send`/`close`/`ping`.

## Released 4.8.2
* Accept compressed messages that end with a BFINAL=1 DEFLATE block (RFC 7692 section 7.2.3.4). Such clients (libdeflate-based and some non-zlib implementations) were disconnected on their first compressed message.

## Released 4.8.1
* Skip the kernel-level cork/uncork (two `setsockopt` calls) around every socket read when write corking is active; those writes leave in the end-of-tick gathered write anyway. Saves 1-2 us of server CPU per read.

## Released 4.8.0
* Prebuilt bindings now compress with zlib-ng 2.2.4 (vendored in `deps/zlib-ng`, statically linked, native `zng_` API so it cannot collide with Node's zlib). Measured on a real RPC stream: level 1 costs 2.2-2.5 us/KB vs 5.6-8 us/KB with zlib. The node-gyp fallback build keeps using Node's zlib through the same `src/Zlib.cpp` wrapper.
* New `perMessageDeflate.level` option (default 2) and a `zlibBackend` export naming the compiled-in implementation.
* Fix the node-gyp fallback build (`npm install` on a platform without a prebuilt binding): it lacked the vendored Node header include path, `HAVE_OPENSSL`/`NODE_WANT_INTERNALS`, and a macOS 10.15 deployment target, so it has failed silently since the Node 20 headers were introduced.

## Released 4.7.0
* permessage-deflate: messages are now compressed by default once negotiated (previously only `send(..., { compress: true })` compressed anything). New options `threshold` (minimum size to compress), `windowBits` and `memLevel` (per-socket memory tier for the sliding window, advertised as `server_max_window_bits` when below 15).
* Remove a leftover debug `console.log` on module load.

## Released 4.6.0
* Add Node 26 support (ABI 147, V8 14.6): vendored `src/headers/26`, `MakeCallback` with explicit `async_context`, tagged internal-field reads via `BaseObject::FromJSObject`.
* Cork writes per event-loop iteration: sends to the same socket within one tick go out in a single gathered write. Disable with `CWS_CORK=0`.
* Binary messages are detached after the `message` handler returns. A retained `ArrayBuffer` now throws on use instead of exposing another connection's bytes (the receive buffer is shared). Copy with `slice()` if the data is needed later.
* Fix use-after-free when a send callback closes the socket while its write queue is draining.
* `terminate()` on an already closed socket is a no-op.
* The `message` handler is now dispatched through `MakeCallback` like every other handler: correct AsyncLocalStorage propagation, exceptions reach `uncaughtException`, and microtasks drain after each message instead of after the whole poll phase. No measurable throughput cost.
* Fix `send()` of a typed array that is a subarray (non-zero `byteOffset`): the bytes were read from the start of the underlying ArrayBuffer instead of the view's offset.

## Released 4.1.0
* Add support for socket.bufferedAmount

## Released 4.0.2
* Add support for Node 16

## Released 3.0.0

**Changes**
* Add support for Node 14 [#42](https://github.com/ClusterWS/cWS/pull/42) 
* Rebuild Node 12 bindings from 12.8.2
* Clarify `Supported Node Versions` in README

## Released 2.0.0

**Changes**
* Drop support for Node 8,9,11
* Fix issues with latest Node 12
* Add "Supported Node Versions" section in README

## Released 1.6.0

**Changes**
* If no close code provided from client return `1005` (similar to ws.js module)

**Bugs**
* Fix [invalid UTF-8 sequence bug](https://github.com/ClusterWS/cWS/issues/39)

## Released 1.5.0

**Changes**
* Fix SSL support on Node 13.9+ [#37](https://github.com/ClusterWS/cWS/pull/37)
* Abort connection on invalid Sec-WebSocket-Key header [#35](https://github.com/ClusterWS/cWS/pull/35)

## Released 1.4.0

**Changes**
* Return `secureProtocol` to docs (stick with tls 1.2)
* Downgrade multiple listeners error to warning (allow listeners overwrite)

## Released 1.3.1

**Improvement**
* Remove `secureProtocol` from required options on ssl

## Released 1.3.0

**Improvement**
* Add SSL support to node 10,11,12,13 (outstanding issue with node 13.9.0 use 13.8.0 instead)

## Released 1.2.0

**Improvement**
* Do not register httpServer on `error` event if server has been passed from the user

## Released 1.1.2

**Fixes**
* Fix typings for on `connection` event

## Released 1.1.0

**Improvement**
* Added support for on `close` event on the `WebSocketServer`

**Fixes**
* Validation prints warning if listener is not supported

## Released 1.0.0

This is quite a big release with some important changes, improvement and fixes including but not limited to:

**Improvement**
* Added `noServer` config
* Added `clients` getter to `WebSocketServer`
* Added `handleUpgrade` similar to `ws` module
* Changed values of `OPEN` and `CLOSED` on `WebSocket` to `1` and `3` respectively
* Reexported `WebSocketServer` under `WebSocket.Server`

**Fixes**
* Fixed `perMessageDeflate` configuration
* Fixed close code on fuzzing

**Removed**
* Removed `global.cws` config
* Removed `websocket.remoteAddress` as can get data from `websocket._socket.remoteAddress` or `req.connection.remoteAddress`
* No more `listening` event emitted from `WebSocketServer` (can be implemented using callback)

Many other fixes and improvements...

## Release 0.17.0

* Remove support for SSL from Node.js 10+ (use proxy instead like nginx)
* Added support for Node.js 13

## Release 0.16.0

* Improved typings for `on('connection')` handler [#25](https://github.com/ClusterWS/cWS/pull/25)
* Improved typings for `verifyClient` [#24](https://github.com/ClusterWS/cWS/pull/24)
* On `verifyClient` fail by default return code `401` [#24](https://github.com/ClusterWS/cWS/pull/24)

## Release 0.15.0
#### Improvements
* `socket.send(buffer, { binary: false })` will force text `opCode`

## Release 0.14.0
#### Improvements
* Adjust `verifyClient` to return the same `info` object as `ws` module instead of `headers` return origin as headers can be accessed from `req`

## Release 0.13.0
#### Improvements
* Add support for node 12

## Release 0.12.2
#### Improvements
* Re throw error from cWS bindings.

