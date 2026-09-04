## Released 4.14.0
* Send worker, four follow-ups from the 2026-09-03 reviews. (1) A clean completion (everything written, no callback, socket not closed, nothing queued behind it) no longer wakes an idle loop; it waits for the loop's next natural wake, so request-response traffic stops paying an eventfd write, an epoll return and an extra loop iteration per message: echo 4 KB 9.1 -> 7.7 µs of JS thread on the EPYC. Anything that must be reconciled promptly (partial write, error, callback, a socket closed or written to while its op was in flight) still wakes. (2) Completions are dequeued in batches and prefetched before any is touched, so the cross-core miss per socket overlaps instead of serializing: the 120 fps render loop's p99 frame lateness drops from 0.75 to 0.16 ms at 96k sends/s and from 4.3 to 1.9 ms at 6.1M sends/s. (3) A socket whose kernel buffer was full is drained by the worker when it becomes writable again instead of by the JS thread one syscall per message plus two cork toggles; without the worker the drain is one gathered write per round, cork-free. (4) The compressed path no longer allocates across threads: the raw payload lives in a pool block the main thread frees (or, above 16 KB, on the heap kept until release), and the worker writes compressed frames into a scratch arena that belongs to the op; a partially sent scratch frame is copied to the heap before it is requeued.
* Prepared messages: a body of up to 4 KB (every compressed blob is one) is copied into the slab after its header instead of borrowed, so the send is a plain slab append with no message node, second iovec or reference-count traffic; a send with a callback keeps the borrowed path. Render loop at 1.5M 256 B prepared sends/s: 58% -> 27% JS thread, now equal to plain sends. With compression the difference is decisive: at 384k 2 KB sends/s one worker thread cannot compress every send (it saturates near 200k 2 KB deflates/s and the JS thread rises to 46% under the backpressure), while prepared messages compress once per frame and hold 120 fps at 25% JS thread with egress halved; at 6.1M 256 B sends/s per-send compression collapses to 80 fps, prepared compression holds 120 fps at 53%.
* microdeflate 20% faster, output unchanged (EPYC 9454P, RPC capture 1.71 -> 1.36 µs/KB, 24 KB messages 1.48 -> 1.21, text 1.40 -> 1.11, random 1.73 -> 1.44; branch mispredictions -40%). The hash entry packs the tag above a 24-bit position so "same tag, older, within 32 KB" is one subtraction and one compare with no second array; literals are processed in pairs in an inner loop the match path never enters; the first 32 bytes of a match are compared with SSE2 byte compares reduced to one mask (portable 8-byte xor selects elsewhere); the length symbol and the distance code are precomputed per length / per code so a match is two bit-writer puts. Measured and rejected on the same host: x86-64-v3 clones (BMI2/AVX2 buy nothing on Zen 4), PGO (at the noise floor), -O2, branch hints on the literal path, branch-free distance codes, 16-bit tables, looping 16-byte SIMD compares.
* `perMessageDeflate.level` now defaults to 1 (was 2). Shared mode always used microdeflate regardless of level; with takeover, level 1 is the microdeflate window and level 2 and above is zlib-ng, so the default takeover configuration is now the 64 KB per-connection window rather than zlib-ng's ~256 KB.
* Context takeover with microdeflate: `perMessageDeflate: { serverNoContextTakeover: false, level: 1 }` now compresses with a per-connection microdeflate window (32 KB history, 64 KB per connection in total) instead of zlib-ng, so a message references the messages before it on that connection. RPC capture, bytes on the wire: shared 2.84x -> takeover 3.50x (19% fewer bytes), at lower CPU per message than shared mode (2.8 vs 3.5 µs on the replay, the appended copy replaces the per-message setup). zlib-ng stays behind level 2 (3.98x at 3x the CPU and ~256 KB per connection) and above. Prepared messages keep their once-per-emission compression on takeover connections too: the independently compressed blob is spliced as usual and a history-sync entry behind it appends the raw bytes to the connection's window in queue order (on the worker, or immediately when the worker is off), so the client's and the server's histories stay in step; those bytes are not indexed for later matches. Clients need no change; browsers negotiate takeover by default.
* Five pressure tests cover partial writes to a slow reader (plain and compressed), close and terminate with a burst in flight, messages queued behind an in-flight send when the loop then idles, and ordering across callbacks, prepared payloads and plain sends.

## Released 4.13.0
* Slab messages: a corked socket now frames its tick's plain sends back to back into one 16 KB pool block (the slab at the queue tail) instead of one pooled block plus list node per send. A whole tick is usually one message: no allocation or link per send on the JS thread, one iovec per socket on the worker, one pop on completion. Sends with a callback, prepared payloads (their header/prefix still goes into the slab) and compressed messages keep their own entries, so ordering, partial writes, close and terminate paths are unchanged (four new pressure tests cover them). EPYC 9454P, JS thread, 4.12.0 -> 4.13.0: fan-out 2 KB to 50 sockets 0.65 -> 0.45 µs per delivery, 256 B 0.38 -> 0.35, echo 64 B 5.45 -> 4.95; the 120 fps render loop at 1.5M 256 B sends/s 40% -> 26% JS thread (and whole-process 85% -> 51%, the worker sends one iovec instead of 256), and at 6.1M sends/s it now holds 120 fps at 78% where 4.12.0 dropped to 116 fps at 97%. Pool size classes now go up to 16 KB.
* Fix: the legacy native `sendPrepared` path reused a pool block without initialising its flags.

## Released 4.12.0
* Send worker redesigned around JS-thread time, measured on the production EPYC 9454P with a 120 fps render loop on the JS thread fanning out to 50 sockets: at 384k 2 KB sends/s the loop without the worker drops frames (p99 7.8 ms late, JS thread 88%) while the worker holds every frame under 1 ms at 30%; JS-thread CPU per delivery 1.50 -> 0.97 µs (2 KB), 0.70 -> 0.40 µs (256 B), per echo message 7.35 -> 5.30 µs (64 B), 13.25 -> 9.65 µs (4 KB). The pieces: hand-offs batched per loop iteration (one wake for everything flushed in a hook, one completion wake per batch); completions drained in the loop hooks, with the uv_async used only when the loop is about to block, so a busy loop pays no syscall for them; the worker spins `CWS_SEND_THREAD_SPIN_US` (default 50) before sleeping, so close ticks never pay a futex wake on the JS thread; `SendOp`s reused from a freelist instead of an 8 KB allocation per socket per tick; and the per-loop block pool is now a real per-size-class freelist up to 4 KB (it cached one block per class before), so a sent message's block returns untouched and the next send reuses it instead of malloc. On Linux the worker keeps itself in the JS thread's last-level-cache domain (it re-pins to the L3 siblings of the CPU the JS thread last flushed from, excluding that core and its SMT twin; `CWS_SEND_THREAD_AFFINITY=0` disables): on the 8-CCD EPYC the scheduler otherwise puts the two threads on different dies and every shared line, the op, the queue slots and each payload the worker reads that the JS thread then reuses, crosses the fabric; fan-out of 2 KB to 50 sockets 0.95 -> 0.65 µs of JS-thread CPU per delivery, and the 384k sends/s render loop 47% -> 21% JS thread. Also removed a leftover no-op `MakeCallback` per loop iteration in the check hook, kept the flush vectors' capacity across ticks, and raised the hand-off queues to 65536 entries (beyond the old 4096 sockets per tick the rest silently went out synchronously on the JS thread). Whole-process CPU is higher with the worker on (the hand-off and the spin); `CWS_SEND_THREAD=0` if total CPU matters more than the JS thread. The ceiling in either mode is the JS-side cost of `send()` itself, ~0.26 µs, i.e. ~3.7M sends/s per process.
* Tried and rejected with measurements: io_uring in the worker (with and without SQPOLL) changed nothing on the JS thread, which never touches the ring, and SQPOLL burned a core; Docker's seccomp filter is not a factor in syscall cost on that host (the SRSO Safe RET mitigation is).

## Released 4.11.2
* microdeflate, output unchanged: match symbols write the bit accumulator unconditionally (their sizes vary, so the flush branch mispredicted), literals keep the predictable conditional flush; the 32 KB distance-code table is replaced by a computed code. Profiled with hardware counters on the production EPYC 9454P: 1.84 -> 1.72 µs/KB on the RPC capture (1.58 -> 1.48 on 24 KB messages), 1.13 -> 1.00 on Apple M; incompressible input unchanged. Tried and rejected with measurements: a packed 32 KB hash table (gcc emits partial-register masks on Zen), a single-branch candidate check (an unconditional random load costs more than the mispredicts it saves), `__restrict`, and smaller or larger tables.

## Released 4.11.1
* microdeflate speed-ups, output unchanged: matches extend 8 bytes per step, the bit writer stores 8 bytes at a time, length/distance codes go out with their extra bits in one write, and each hash slot carries an 8-bit tag so a stale candidate is rejected without touching the input. Measured on the RPC capture: 1.74 -> 1.20 µs/KB (1.45x), byte-identical output. Incompressible input (random bytes) now falls back to stored blocks: 3x faster and 0.999 instead of 0.948 ratio.

## Released 4.11.0
* Prepared messages for fan-out: `new PreparedMessage(bytes)` copies a payload into native memory once; `ws.send(prepared, { prefix })` sends `prefix + payload` as one frame without copying or compressing the payload per socket. Uncompressed, the payload goes out as a second gather buffer. Compressed (shared compressor mode), the prefix is emitted as a DEFLATE stored block ahead of the payload's deflate blocks, which are built once on the first compressed send and cached on the handle; standard inflaters accept the result unchanged. Sockets with context takeover and client sockets take the regular copying path. Measured with 300 subscribers and a 3 KB payload: 0.9 -> 0.2 µs of JS-thread time per socket, and no per-socket compression at all.
* `send()` accepts any `ArrayBufferView` (typed arrays are sent from their own offset, no `Buffer.from` copy needed).

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

