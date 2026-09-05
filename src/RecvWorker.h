#ifndef CWS_RECV_WORKER_H
#define CWS_RECV_WORKER_H

// Receive worker thread (optional, default off; `receiveThread: true` per server or
// CWS_RECV_THREAD=1). One thread per loop owns *readability* of plain-TCP server
// WebSockets: it polls them (epoll on Linux, kqueue on macOS), does recv, frame parsing,
// unmasking, fragment reassembly and inflate, and appends complete records to a ring in
// plain native memory. The main thread drains the ring in the loop hooks (see
// beforePoll/afterPoll, mirroring SendWorker) inside the batched read scope, hands out
// zero-copy views over the ring and advances the read index only after those views are
// detached. The worker never writes to a socket and never closes an fd: pongs, close
// handshakes and socket ends run on the main thread through the existing paths.
//
// Ring record: a 16-byte header {u32 length, u8 type, u8 opCode, u8 flags, u8 pad, void
// *socket} followed by the payload padded to 16 bytes; a header with length 0xffffffff is
// a wrap marker (skip to the start of the ring). A record is at most half the ring; larger
// payloads, and any payload while the ring is full, are heap copies referenced from a
// small record (F_HEAP: payload = {char *data, size_t length}) and freed by the drain.
//
// Quiesce protocol: the main thread never closes the fd of an attached socket directly.
// closeSocket enqueues a DEREGISTER; the worker removes the fd from its poller and
// acknowledges with an R_ACK record; the drain then releases the fd (shared with an
// orphaned send op through Socket::FdRelease) and deletes the object once libuv has also
// closed its poll handle. Until the ack the object stays alive and closing, and records
// for it are dropped. A reused fd number can therefore never be read as the old socket.
//
// Backpressure: when the ring cannot take a record the worker parks the socket (EPOLLIN
// off / EV_DISABLE), keeps the record on the heap in per-socket order and sets
// `needResume`; the main thread, after a drain that freed space, sends a RESUME request
// and the worker flushes the parked records and re-enables the sockets.
//
// Wake protocol: main -> worker requests go through an SPSC queue; the worker blocks in
// its poller with `workerSleeping` set (Dekker with the requester, which writes the wake
// eventfd/pipe only when it sees the flag). Worker -> main: records are published with a
// release store; the worker calls uv_async_send only when it sees `loopIdle` (set in
// beforePoll after a final drain, cleared in afterPoll), so a busy loop never pays a
// syscall per batch. Windows: not supported, the option is ignored.

#include <uv.h>
#include <cstddef>

namespace cS {

struct Socket;
struct NodeData;

struct RecvWorker {
    // Starts the worker for `loop` on first call (lazily, from the first group that asks for
    // it). Returns false when disabled by CWS_RECV_THREAD=0 or unsupported (Windows).
    static bool init(uv_loop_t *loop);
    static bool active();
    static const char *status();   // "active", "not started", or why not
    // Whether the option should be honoured: CWS_RECV_THREAD=1 turns it on for every server,
    // =0 turns it off everywhere, unset leaves the per-server option in charge.
    static bool wanted(bool option);

    // Main thread. Hands readability of `fd` (a plain-TCP server WebSocket) to the worker.
    // Returns the connection handle to keep in Socket::recvConn, or nullptr if not active.
    static void *attach(Socket *s, uv_os_sock_t fd, NodeData *nodeData, unsigned int maxPayload, bool perMessageDeflate);
    // Main thread. Asks the worker to stop reading; the ack (an R_ACK record) completes the
    // close through Socket::recvDeregistered.
    static void detach(void *conn);

    // Loop hooks. beforePoll: drains, marks the loop as about to block and drains once more;
    // returns true if anything was delivered so the caller can flush the replies. afterPoll: clears the mark and drains. drain(): deliver
    // everything currently in the ring (used by the wake callback).
    static bool beforePoll();
    static void afterPoll();
    static bool drain();

private:
    // Main thread: one batch of records (inside the read scope) and one record's handling
    // (members so the friend grants on Socket/WebSocket/Group apply).
    static void deliverBatch(void *batch);
    static void dispatch(Socket *s, unsigned char type, unsigned char opCode, char *data, size_t length);
};

}

#endif // CWS_RECV_WORKER_H
