#include "RecvWorker.h"
#include "Socket.h"
#include "Group.h"
#include "Hub.h"
#include "Zlib.h"
#include "readerwriterqueue.h"
#include <atomic>
#include <thread>
#include <vector>
#include <deque>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <cctype>

#if defined(__linux__) || defined(__APPLE__)
#define CWS_RECV_WORKER_SUPPORTED 1
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#else
#include <sys/event.h>
#endif
#endif

namespace cS {

#ifdef CWS_RECV_WORKER_SUPPORTED

namespace {

// ---- ring records -------------------------------------------------------------------

enum RecordType : uint8_t {
    R_MESSAGE = 1,   // opCode 1/2, payload = the message
    R_PING,          // payload = ping payload (main thread answers with a pong)
    R_PONG,
    R_CLOSE,         // payload = raw close frame payload (parsed on the main thread)
    R_EOF,           // recv returned 0
    R_ERROR,         // recv failed, or a protocol violation the parser would have closed on
    R_ACK            // the socket's deregistration is complete
};
enum RecordFlags : uint8_t {
    F_HEAP = 1       // payload is a HeapRef to a heap copy (ring too small or full)
};
struct RecordHeader {
    uint32_t length;
    uint8_t type, opCode, flags, pad;
    void *socket;
};
static_assert(sizeof(RecordHeader) == 16, "ring records are 16-byte aligned");
struct HeapRef {
    char *data;
    size_t length;
};
constexpr uint32_t WRAP = 0xffffffffu;
constexpr size_t HEADER = sizeof(RecordHeader);
// Records delivered per callback scope on the main thread; matches READ_SCOPE_BATCH in Addon.h.
constexpr size_t DRAIN_BATCH = 64;
constexpr size_t LARGE_BUFFER = 300 * 1024;   // recv and inflate scratch, like the hub's

inline size_t roundUp16(size_t n) {
    return (n + 15) & ~(size_t) 15;
}

struct RecvConn;
struct Request {
    enum Type : int { REGISTER, DEREGISTER, RESUME } type;
    RecvConn *conn;
};

// ---- poller ---------------------------------------------------------------------------

struct Poller {
    int fd = -1;
    static const int MAX_EVENTS = 512;
#ifdef __linux__
    epoll_event evs[MAX_EVENTS];
    bool init() { fd = epoll_create1(EPOLL_CLOEXEC); return fd >= 0; }
    void add(int s, void *p, bool read) { epoll_event e{}; e.events = read ? EPOLLIN : 0; e.data.ptr = p; epoll_ctl(fd, EPOLL_CTL_ADD, s, &e); }
    void mod(int s, void *p, bool read) { epoll_event e{}; e.events = read ? EPOLLIN : 0; e.data.ptr = p; epoll_ctl(fd, EPOLL_CTL_MOD, s, &e); }
    void del(int s) { epoll_ctl(fd, EPOLL_CTL_DEL, s, nullptr); }
    int wait() { int n; do { n = epoll_wait(fd, evs, MAX_EVENTS, -1); } while (n < 0 && errno == EINTR); return n < 0 ? 0 : n; }
    void *at(int i) { return evs[i].data.ptr; }
#else
    struct kevent evs[MAX_EVENTS];
    bool init() { fd = kqueue(); return fd >= 0; }
    void add(int s, void *p, bool read) { struct kevent e; EV_SET(&e, s, EVFILT_READ, EV_ADD | (read ? EV_ENABLE : EV_DISABLE), 0, 0, p); kevent(fd, &e, 1, nullptr, 0, nullptr); }
    void mod(int s, void *p, bool read) { struct kevent e; EV_SET(&e, s, EVFILT_READ, read ? EV_ENABLE : EV_DISABLE, 0, 0, p); kevent(fd, &e, 1, nullptr, 0, nullptr); }
    void del(int s) { struct kevent e; EV_SET(&e, s, EVFILT_READ, EV_DELETE, 0, 0, nullptr); kevent(fd, &e, 1, nullptr, 0, nullptr); }
    int wait() { int n; do { n = kevent(fd, nullptr, 0, evs, MAX_EVENTS, nullptr); } while (n < 0 && errno == EINTR); return n < 0 ? 0 : n; }
    void *at(int i) { return evs[i].udata; }
#endif
};

// ---- per-connection worker state -------------------------------------------------------

// Created on the main thread (attach), then owned by the worker until it has processed the
// DEREGISTER request; freed by the main thread when it drains the ack. The parser state
// lives in the WebSocketState base so WebSocketProtocol<SERVER, RecvConn> can drive it.
struct RecvConn : cWS::WebSocketState<true> {
    // set before hand-off, read-only afterwards
    uv_os_sock_t fd = -1;
    Socket *ws = nullptr;
    NodeData::SendStats *stats = nullptr;
    unsigned int maxPayload = 0;
    enum CompressionStatus : char { DISABLED, ENABLED, COMPRESSED_FRAME } compressionStatus = DISABLED;

    // worker-owned
    std::string fragmentBuffer;
    unsigned char controlTipLength = 0;
    bool polled = false;          // registered with the poller
    bool closeReceived = false;   // a close frame was handed over: later bytes are ignored, like onData while shutting down
    bool dead = false;            // EOF/error/protocol error handed over, or deregistered: never read again
    bool parked = false;          // polling disabled while records wait for ring space
    bool ackPending = false;      // deregistered, ack could not be appended yet
    struct Deferred {
        uint8_t type, opCode;
        char *data;
        size_t length;
    };
    std::deque<Deferred> deferred;   // records in order, waiting for ring space (heap copies)

    // WebSocketProtocol Impl
    static bool refusePayloadLength(uint64_t length, cWS::WebSocketState<true> *wState);
    static bool setCompressed(cWS::WebSocketState<true> *wState);
    static void forceClose(cWS::WebSocketState<true> *wState);
    static bool handleFragment(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, cWS::WebSocketState<true> *wState);
};

typedef cWS::WebSocketProtocol<cWS::SERVER, RecvConn> Protocol;

// ---- the worker ------------------------------------------------------------------------

struct Worker {
    uv_loop_t *loop = nullptr;
    // ring (plain native memory); indices are free-running byte counters, position = index & mask
    char *ring = nullptr;
    size_t size = 0, mask = 0;
    alignas(64) std::atomic<uint64_t> writeIndex{0};   // worker publishes, main reads
    alignas(64) std::atomic<uint64_t> readIndex{0};    // main publishes, worker reads
    alignas(64) std::atomic<bool> needResume{false};   // worker: waiting for ring space
    alignas(64) std::atomic<bool> loopIdle{false};     // main: about to block in poll
    alignas(64) std::atomic<bool> workerSleeping{false}; // worker: about to block in the poller
    alignas(64) moodycamel::ReaderWriterQueue<Request> requests{1024};

    // worker thread only
    uint64_t w = 0;
    size_t pendingNeed = 0;
    bool appended = false;
    NodeData::SendStats *lastStats = nullptr;
    Poller poller;
    int wakeRd = -1, wakeWr = -1;
    std::vector<RecvConn *> parkedList;
    char *recvBlock = nullptr, *recvBuffer = nullptr;
    unsigned int recvLength = 0;
    cWS::zlib::Stream *inflateStream = nullptr;
    char *zlibBuffer = nullptr;
    std::string dynamic;

    // main thread only
    uv_async_t mainWake;
    size_t attached = 0;
    void (*readBatchHook)(void *, void (*)(void *)) = nullptr;
    bool draining = false;
    std::vector<char *> heapFree;

    // ---- ring, worker side
    bool fitsRing(size_t len) {
        return HEADER + roundUp16(len) <= size / 2;
    }
    // Reserves a record; nullptr if the ring has no room (needResume is then set, Dekker
    // with the drain: it advances readIndex, fences and checks needResume; we set
    // needResume, fence and re-read readIndex, so one side sees the other).
    char *reserve(void *socket, uint8_t type, uint8_t opCode, uint8_t flags, uint32_t len, NodeData::SendStats *stats) {
        size_t need = HEADER + roundUp16(len);
        size_t pos = (size_t) (w & mask);
        bool wrap = pos + need > size;
        size_t total = wrap ? (size - pos) + need : need;
        uint64_t r = readIndex.load(std::memory_order_acquire);
        if (size - (size_t) (w - r) < total) {
            needResume.store(true, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            r = readIndex.load(std::memory_order_seq_cst);
            if (size - (size_t) (w - r) < total) {
                return nullptr;
            }
        }
        if (wrap) {
            RecordHeader *marker = (RecordHeader *) (ring + pos);   // at least 16 bytes remain at any 16-aligned pos
            marker->length = WRAP;
            marker->type = 0;
            marker->opCode = 0;
            marker->flags = 0;
            marker->pad = 0;
            marker->socket = nullptr;
            w += size - pos;
            pos = 0;
        }
        RecordHeader *h = (RecordHeader *) (ring + pos);
        h->length = len;
        h->type = type;
        h->opCode = opCode;
        h->flags = flags;
        h->pad = 0;
        h->socket = socket;
        pendingNeed = need;
        lastStats = stats;
        return ring + pos + HEADER;
    }
    void commit() {
        w += pendingNeed;
        writeIndex.store(w, std::memory_order_release);
        appended = true;
    }
    bool emitHeap(RecvConn *c, uint8_t type, uint8_t opCode, char *heap, size_t len) {
        char *p = reserve(c->ws, type, opCode, F_HEAP, (uint32_t) sizeof(HeapRef), c->stats);
        if (!p) {
            return false;
        }
        HeapRef ref{heap, len};
        memcpy(p, &ref, sizeof(ref));
        commit();
        return true;
    }
    // Appends a record for c, in order: into the ring when it fits, else as a heap copy
    // referenced from a small record, else parked behind the ring until the drain frees space.
    void emit(RecvConn *c, uint8_t type, uint8_t opCode, const char *data, size_t len) {
        if (c->deferred.empty()) {
            if (fitsRing(len)) {
                char *p = reserve(c->ws, type, opCode, 0, (uint32_t) len, c->stats);
                if (p) {
                    if (len) {
                        memcpy(p, data, len);
                    }
                    commit();
                    return;
                }
            }
            char *heap = new char[len ? len : 1];
            if (len) {
                memcpy(heap, data, len);
            }
            if (emitHeap(c, type, opCode, heap, len)) {
                return;
            }
            c->deferred.push_back({type, opCode, heap, len});
        } else {
            char *heap = new char[len ? len : 1];
            if (len) {
                memcpy(heap, data, len);
            }
            c->deferred.push_back({type, opCode, heap, len});
        }
        park(c);
    }
    bool emitDeferred(RecvConn *c, RecvConn::Deferred &d) {
        if (fitsRing(d.length)) {
            char *p = reserve(c->ws, d.type, d.opCode, 0, (uint32_t) d.length, c->stats);
            if (p) {
                if (d.length) {
                    memcpy(p, d.data, d.length);
                }
                commit();
                delete [] d.data;
                return true;
            }
            return false;
        }
        return emitHeap(c, d.type, d.opCode, d.data, d.length);
    }
    bool emitAck(RecvConn *c) {
        // After commit() the main thread may free c at any moment: nothing below touches it.
        char *p = reserve(c->ws, R_ACK, 0, 0, 0, c->stats);
        if (!p) {
            return false;
        }
        commit();
        return true;
    }
    void emitMessage(RecvConn *c, int opCode, const char *data, size_t len) {
        c->stats->recvWorkerMessages.fetch_add(1, std::memory_order_relaxed);
        emit(c, R_MESSAGE, (uint8_t) opCode, data, len);
    }
    // Once per poller batch: publish and, only if the loop is about to block, wake it.
    void endOfBatch() {
        if (!appended) {
            return;
        }
        appended = false;
        // Dekker with beforePoll(): either it sees our records, or we see loopIdle
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (loopIdle.load(std::memory_order_seq_cst)) {
            if (lastStats) {
                lastStats->recvWorkerWakes.fetch_add(1, std::memory_order_relaxed);
            }
            uv_async_send(&mainWake);
        }
    }

    // ---- sockets, worker side
    void park(RecvConn *c) {
        if (c->parked) {
            return;
        }
        c->parked = true;
        c->stats->recvStalls.fetch_add(1, std::memory_order_relaxed);
        if (c->polled) {
            poller.mod(c->fd, c, false);
        }
        parkedList.push_back(c);
    }
    void unpark(RecvConn *c) {
        for (size_t i = 0; i < parkedList.size(); i++) {
            if (parkedList[i] == c) {
                parkedList[i] = parkedList.back();
                parkedList.pop_back();
                break;
            }
        }
        c->parked = false;
    }
    // RESUME: the drain freed space. Flush parked records in order, then poll the sockets again.
    void resumeParked() {
        std::vector<RecvConn *> list;
        list.swap(parkedList);
        for (RecvConn *c : list) {
            if (c->ackPending) {
                if (!emitAck(c)) {
                    parkedList.push_back(c);
                }
                continue;
            }
            while (!c->deferred.empty()) {
                if (!emitDeferred(c, c->deferred.front())) {
                    break;
                }
                c->deferred.pop_front();
            }
            if (!c->deferred.empty()) {
                parkedList.push_back(c);
                continue;
            }
            c->parked = false;
            if (c->polled) {
                poller.mod(c->fd, c, true);
            }
        }
    }
    void end(RecvConn *c, uint8_t type) {
        if (c->dead) {
            return;
        }
        c->dead = true;
        if (c->polled) {
            poller.del(c->fd);
            c->polled = false;
        }
        emit(c, type, 0, nullptr, 0);
    }
    char *inflate(char *data, size_t &length, size_t maxPayload) {
        return cWS::zlib::inflate(inflateStream, data, length, maxPayload, zlibBuffer, LARGE_BUFFER, dynamic);
    }
    void onReadable(RecvConn *c) {
        if (c->dead || c->parked) {
            return;
        }
        ssize_t n = ::recv(c->fd, recvBuffer, recvLength, 0);
        if (n > 0) {
            c->stats->reads.fetch_add(1, std::memory_order_relaxed);
            if (!c->closeReceived) {
                Protocol::consume(recvBuffer, (unsigned int) n, c);
            }
        } else if (n == 0) {
            end(c, R_EOF);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            end(c, R_ERROR);
        }
    }
    void deregister(RecvConn *c) {
        if (c->polled) {
            poller.del(c->fd);
            c->polled = false;
        }
        c->dead = true;
        for (RecvConn::Deferred &d : c->deferred) {
            delete [] d.data;
        }
        c->deferred.clear();
        if (c->parked) {
            unpark(c);
        }
        if (!emitAck(c)) {
            c->ackPending = true;
            parkedList.push_back(c);
        }
    }
    void processRequests() {
        Request req;
        while (requests.try_dequeue(req)) {
            switch (req.type) {
            case Request::REGISTER:
                poller.add(req.conn->fd, req.conn, true);
                req.conn->polled = true;
                break;
            case Request::DEREGISTER:
                deregister(req.conn);
                break;
            case Request::RESUME:
                resumeParked();
                break;
            }
        }
    }
    void drainWakeFd() {
        char buf[64];
        while (::read(wakeRd, buf, sizeof(buf)) > 0) {}
    }
    void run() {
#ifdef __linux__
        pthread_setname_np(pthread_self(), "cws-recv");
#else
        pthread_setname_np("cws-recv");
#endif
        for (;;) {
            processRequests();
            endOfBatch();   // acks and resumed records
            // Dekker with request(): either it sees workerSleeping and writes the wake fd, or
            // we see its request before blocking.
            workerSleeping.store(true, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (requests.peek()) {
                workerSleeping.store(false, std::memory_order_seq_cst);
                continue;
            }
            int n = poller.wait();
            workerSleeping.store(false, std::memory_order_seq_cst);
            for (int i = 0; i < n; i++) {
                void *p = poller.at(i);
                if (!p) {
                    drainWakeFd();
                } else {
                    onReadable((RecvConn *) p);
                }
            }
            endOfBatch();
        }
    }

    // ---- main thread side
    void request(Request req) {
        requests.enqueue(req);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (workerSleeping.load(std::memory_order_seq_cst)) {
#ifdef __linux__
            uint64_t one = 1;
            (void) !::write(wakeWr, &one, sizeof(one));
#else
            char one = 1;
            (void) !::write(wakeWr, &one, 1);
#endif
        }
    }
};

Worker *g = nullptr;
const char *workerStatus = "not started";

// ---- WebSocketProtocol Impl (worker thread) ----------------------------------------------

bool RecvConn::refusePayloadLength(uint64_t length, cWS::WebSocketState<true> *wState) {
    return length > static_cast<RecvConn *>(wState)->maxPayload;
}

bool RecvConn::setCompressed(cWS::WebSocketState<true> *wState) {
    RecvConn *c = static_cast<RecvConn *>(wState);
    if (c->compressionStatus == ENABLED) {
        c->compressionStatus = COMPRESSED_FRAME;
        return true;
    }
    return false;
}

void RecvConn::forceClose(cWS::WebSocketState<true> *wState) {
    // what WebSocket::terminate() does on the loop: the main thread ends the socket (1006)
    g->end(static_cast<RecvConn *>(wState), R_ERROR);
}

// Mirror of WebSocket<SERVER>::handleFragment with records instead of handler calls.
bool RecvConn::handleFragment(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, cWS::WebSocketState<true> *wState) {
    RecvConn *c = static_cast<RecvConn *>(wState);
    Worker *wk = g;
    if (opCode < 3) {
        if (!remainingBytes && fin && !c->fragmentBuffer.length()) {
            if (c->compressionStatus == COMPRESSED_FRAME) {
                c->compressionStatus = ENABLED;
                data = wk->inflate(data, length, c->maxPayload);
                if (!data) {
                    forceClose(wState);
                    return true;
                }
            }
            if (opCode == 1 && !Protocol::isValidUtf8((unsigned char *) data, length)) {
                forceClose(wState);
                return true;
            }
            wk->emitMessage(c, opCode, data, length);
        } else {
            c->fragmentBuffer.append(data, length);
            if (!remainingBytes && fin) {
                length = c->fragmentBuffer.length();
                if (c->compressionStatus == COMPRESSED_FRAME) {
                    c->compressionStatus = ENABLED;
                    c->fragmentBuffer.append("....");
                    data = wk->inflate((char *) c->fragmentBuffer.data(), length, c->maxPayload);
                    if (!data) {
                        forceClose(wState);
                        return true;
                    }
                } else {
                    data = (char *) c->fragmentBuffer.data();
                }
                if (opCode == 1 && !Protocol::isValidUtf8((unsigned char *) data, length)) {
                    forceClose(wState);
                    return true;
                }
                wk->emitMessage(c, opCode, data, length);
                c->fragmentBuffer.clear();
            }
        }
    } else {
        if (!remainingBytes && fin && !c->controlTipLength) {
            if (opCode == cWS::CLOSE) {
                wk->emit(c, R_CLOSE, 0, data, length);
                c->closeReceived = true;
                return true;
            } else if (opCode == cWS::PING) {
                wk->emit(c, R_PING, 0, data, length);
            } else if (opCode == cWS::PONG) {
                wk->emit(c, R_PONG, 0, data, length);
            }
        } else {
            c->fragmentBuffer.append(data, length);
            c->controlTipLength += length;
            if (!remainingBytes && fin) {
                char *controlBuffer = (char *) c->fragmentBuffer.data() + c->fragmentBuffer.length() - c->controlTipLength;
                if (opCode == cWS::CLOSE) {
                    wk->emit(c, R_CLOSE, 0, controlBuffer, c->controlTipLength);
                    c->closeReceived = true;
                    return true;
                } else if (opCode == cWS::PING) {
                    wk->emit(c, R_PING, 0, controlBuffer, c->controlTipLength);
                } else if (opCode == cWS::PONG) {
                    wk->emit(c, R_PONG, 0, controlBuffer, c->controlTipLength);
                }
                c->fragmentBuffer.resize(c->fragmentBuffer.length() - c->controlTipLength);
                c->controlTipLength = 0;
            }
        }
    }
    return false;
}

// ---- delivery (main thread) ---------------------------------------------------------------

typedef cWS::WebSocket<cWS::SERVER> ServerSocket;

}

void RecvWorker::dispatch(Socket *s, unsigned char type, unsigned char opCode, char *data, size_t length) {
    ServerSocket *ws = static_cast<ServerSocket *>(s);
    if (type == R_ACK) {
        RecvConn *c = (RecvConn *) s->recvConn;
        delete c;
        if (--g->attached == 0) {
            uv_unref((uv_handle_t *) &g->mainWake);
        }
        Socket::recvDeregistered(s);   // may delete s
        return;
    }
    if (ws->isClosed()) {
        return;   // closing (ack pending) or ended: nothing more is delivered
    }
    if (type == R_EOF || type == R_ERROR) {
        ServerSocket::onEnd(ws);
        return;
    }
    if (ws->isShuttingDown()) {
        return;   // close() was sent: incoming frames are ignored, as onData does
    }
    ws->hasOutstandingPong = false;
    cWS::Group<cWS::SERVER> *group = cWS::Group<cWS::SERVER>::from(ws);
    switch (type) {
    case R_MESSAGE:
        group->messageHandler(ws, data, length, (cWS::OpCode) opCode);
        break;
    case R_PING:
        ws->send(data, length, cWS::OpCode::PONG);
        group->pingHandler(ws, data, length);
        break;
    case R_PONG:
        group->pongHandler(ws, data, length);
        break;
    case R_CLOSE: {
        cWS::WebSocketProtocol<cWS::SERVER, ServerSocket>::CloseFrame closeFrame = cWS::WebSocketProtocol<cWS::SERVER, ServerSocket>::parseClosePayload(data, length);
        ws->close(closeFrame.code, closeFrame.message, closeFrame.length);
        break;
    }
    default:
        break;
    }
}

namespace {
struct Batch {
    uint64_t cursor;
    size_t delivered;
};
}

// One batch of records from `cursor`; runs inside the read scope when there is one.
void RecvWorker::deliverBatch(void *arg) {
    Batch *b = (Batch *) arg;
    Worker *wk = g;
    uint64_t end = wk->writeIndex.load(std::memory_order_acquire);
    while (b->cursor != end && b->delivered < DRAIN_BATCH) {
        size_t pos = (size_t) (b->cursor & wk->mask);
        RecordHeader *h = (RecordHeader *) (wk->ring + pos);
        if (h->length == WRAP) {
            b->cursor += wk->size - pos;
            continue;
        }
        char *payload = wk->ring + pos + HEADER;
        size_t length = h->length;
        b->cursor += HEADER + roundUp16(h->length);
        if (h->flags & F_HEAP) {
            HeapRef ref;
            memcpy(&ref, payload, sizeof(ref));
            payload = ref.data;
            length = ref.length;
            wk->heapFree.push_back(ref.data);   // freed after the batch's views are detached
        }
        b->delivered++;
        dispatch((Socket *) h->socket, h->type, h->opCode, payload, length);
    }
}

bool RecvWorker::init(uv_loop_t *loop) {
    if (g) {
        return true;
    }
    size_t ringKb = 8192;
    const char *env = getenv("CWS_RECV_RING_KB");
    if (env && *env) {
        long v = atol(env);
        if (v >= 16) {
            ringKb = (size_t) v;
        }
    }
    size_t size = 1;
    while (size < ringKb * 1024) {
        size <<= 1;
    }
    Worker *wk = new Worker();
    wk->loop = loop;
    void *mem = nullptr;
    if (posix_memalign(&mem, 64, size) != 0 || !wk->poller.init()) {
        workerStatus = "failed to allocate";
        delete wk;
        return false;
    }
    wk->ring = (char *) mem;
    wk->size = size;
    wk->mask = size - 1;
#ifdef __linux__
    wk->wakeRd = wk->wakeWr = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
    int p[2] = {-1, -1};
    if (pipe(p) == 0) {
        fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
        fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);
        fcntl(p[0], F_SETFD, FD_CLOEXEC);
        fcntl(p[1], F_SETFD, FD_CLOEXEC);
    }
    wk->wakeRd = p[0];
    wk->wakeWr = p[1];
#endif
    if (wk->wakeRd < 0) {
        workerStatus = "failed to create the wake fd";
        free(mem);
        delete wk;
        return false;
    }
    wk->poller.add(wk->wakeRd, nullptr, true);
    // recv buffer with the parser's padding, like the hub's
    wk->recvBlock = new char[LARGE_BUFFER];
    wk->recvBuffer = wk->recvBlock + Protocol::CONSUME_PRE_PADDING;
    wk->recvLength = (unsigned int) (LARGE_BUFFER - Protocol::CONSUME_PRE_PADDING - Protocol::CONSUME_POST_PADDING);
    wk->inflateStream = cWS::zlib::createInflate(15);
    wk->zlibBuffer = new char[LARGE_BUFFER];
    uv_async_init(loop, &wk->mainWake, [](uv_async_t *) {
        // the loop is awake: the check hook (afterPoll) drains and flushes the replies
    });
    uv_unref((uv_handle_t *) &wk->mainWake);   // ref'd while sockets are attached (see attach)
    g = wk;
    std::thread([wk] { wk->run(); }).detach();
    workerStatus = "active";
    return true;
}

bool RecvWorker::active() {
    return g != nullptr;
}

const char *RecvWorker::status() {
    return workerStatus;
}

bool RecvWorker::wanted(bool option) {
    const char *value = getenv("CWS_RECV_THREAD");
    if (!value || !*value) {
        return option;
    }
    std::string v(value);
    for (char &c : v) {
        c = (char) tolower((unsigned char) c);
    }
    if (v == "0" || v == "false" || v == "off" || v == "no") {
        workerStatus = "disabled by CWS_RECV_THREAD";
        return false;
    }
    return true;
}

void *RecvWorker::attach(Socket *s, uv_os_sock_t fd, NodeData *nodeData, unsigned int maxPayload, bool perMessageDeflate) {
    Worker *wk = g;
    if (!wk) {
        return nullptr;
    }
    RecvConn *c = new RecvConn();
    c->fd = fd;
    c->ws = s;
    c->stats = nodeData->sendStats;
    c->maxPayload = maxPayload;
    c->compressionStatus = perMessageDeflate ? RecvConn::ENABLED : RecvConn::DISABLED;
    wk->readBatchHook = nodeData->readBatchHook;
    if (wk->attached++ == 0) {
        uv_ref((uv_handle_t *) &wk->mainWake);   // an attached socket keeps the loop alive, as its poll handle did
    }
    wk->request({Request::REGISTER, c});
    return c;
}

void RecvWorker::detach(void *conn) {
    g->request({Request::DEREGISTER, (RecvConn *) conn});
}

bool RecvWorker::drain() {
    Worker *wk = g;
    if (!wk || wk->draining) {
        return false;
    }
    wk->draining = true;
    bool any = false;
    uint64_t r = wk->readIndex.load(std::memory_order_relaxed);
    while (r != wk->writeIndex.load(std::memory_order_acquire)) {
        Batch b{r, 0};
        if (wk->readBatchHook) {
            wk->readBatchHook(&b, deliverBatch);   // views handed out are detached before this returns
        } else {
            deliverBatch(&b);                       // per-message scopes detach as they go
        }
        r = b.cursor;
        wk->readIndex.store(r, std::memory_order_release);
        for (char *p : wk->heapFree) {
            delete [] p;
        }
        wk->heapFree.clear();
        any = true;
    }
    wk->draining = false;
    if (any) {
        // Dekker with reserve(): see there
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (wk->needResume.exchange(false, std::memory_order_seq_cst)) {
            wk->request({Request::RESUME, nullptr});
        }
    }
    return any;
}

bool RecvWorker::beforePoll() {
    Worker *wk = g;
    if (!wk) {
        return false;
    }
    // Always mark the loop idle here, even when the poll is not going to block (pending
    // immediates): the public uv_backend_timeout() is not what uv_run computes (it returns 0
    // whenever io watcher changes are queued), so it cannot tell the two apart. A wake for a
    // poll that returns at once only costs the worker a uv_async_send.
    bool any = drain();
    wk->loopIdle.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (drain()) {
        any = true;
    }
    return any;
}

void RecvWorker::afterPoll() {
    Worker *wk = g;
    if (!wk) {
        return;
    }
    wk->loopIdle.store(false, std::memory_order_seq_cst);
    drain();
}

#else // !CWS_RECV_WORKER_SUPPORTED (Windows): the option is ignored

bool RecvWorker::init(uv_loop_t *) { return false; }
bool RecvWorker::active() { return false; }
const char *RecvWorker::status() { return "not supported on this platform"; }
bool RecvWorker::wanted(bool) { return false; }
void *RecvWorker::attach(Socket *, uv_os_sock_t, NodeData *, unsigned int, bool) { return nullptr; }
void RecvWorker::detach(void *) {}
bool RecvWorker::beforePoll() { return false; }
void RecvWorker::afterPoll() {}
bool RecvWorker::drain() { return false; }

#endif

}
