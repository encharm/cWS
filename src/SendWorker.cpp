#include "SendWorker.h"
#include <vector>
#include "Socket.h"
#include "Zlib.h"
#include "cWS.h"
#include "readerwriterqueue.h"
#include <thread>
#include <atomic>
#include <chrono>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef __linux__
#include <sched.h>
#include <fstream>
#include <sstream>
#endif
#include <cstdlib>
#include <cstring>
#include <string>
#include <cerrno>
#include <cctype>
#include <string>

namespace cS {

namespace {
    bool workerActive = false;
    const char *workerStatus = "not initialised";
    moodycamel::ReaderWriterQueue<Socket::SendOp *> *toWorker = nullptr;
    moodycamel::ReaderWriterQueue<Socket::SendOp *> *toMain = nullptr;
    moodycamel::spsc_sema::LightweightSemaphore *workerSema = nullptr;
    uv_async_t mainWake;
    bool signalPending = false;                 // main thread: submit() since the last flush()
    std::vector<Socket::SendOp *> freeOps;      // main thread only
    std::atomic<bool> loopIdle{false};          // main: set before blocking in poll, cleared after
    std::atomic<bool> workerSleeping{false};    // worker: set before blocking on the semaphore
    int spinMicros = 50;
    std::atomic<int> jsCpu{-1};                 // main: the CPU the JS thread last flushed from
    bool affinityEnabled = true;

#ifdef __linux__
    // Keeps the worker in the JS thread's last-level-cache domain (Linux). Every cache line the
    // two threads share, the op, the queue slots and every payload the worker reads that the
    // JS thread then reuses from the freelist, stays in one L3 instead of crossing the fabric.
    // Measured on an EPYC 9454P: JS-thread time per message roughly halves. The JS thread is not
    // pinned; when it moves to another domain the worker follows on its next batch.
    std::vector<int> parseCpuList(const std::string &list) {
        std::vector<int> cpus; std::stringstream ss(list); std::string part;
        while (std::getline(ss, part, ',')) {
            size_t dash = part.find('-');
            int lo = atoi(part.c_str()), hi = dash == std::string::npos ? lo : atoi(part.c_str() + dash + 1);
            for (int c = lo; c <= hi && c >= 0; c++) cpus.push_back(c);
        }
        return cpus;
    }
    std::vector<int> readCpuList(int cpu, const char *file) {
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + file);
        std::string line; std::getline(in, line);
        return parseCpuList(line);
    }
    int followedCpu = -1;
    cpu_set_t allowedAtStart;
    void followJsThread() {
        int cpu = jsCpu.load(std::memory_order_relaxed);
        if (cpu < 0 || cpu == followedCpu) return;
        followedCpu = cpu;
        std::vector<int> llc = readCpuList(cpu, "/cache/index3/shared_cpu_list");
        std::vector<int> siblings = readCpuList(cpu, "/topology/thread_siblings_list");
        cpu_set_t mask; CPU_ZERO(&mask); int n = 0;
        for (int c : llc) {
            bool sibling = false;
            for (int sib : siblings) if (sib == c) sibling = true;   // not the JS core or its SMT twin
            if (!sibling && CPU_ISSET(c, &allowedAtStart)) { CPU_SET(c, &mask); n++; }
        }
        if (n == 0) for (int c : llc) if (CPU_ISSET(c, &allowedAtStart)) { CPU_SET(c, &mask); n++; }
        if (n > 0) sched_setaffinity(0, sizeof(mask), &mask);
    }
#endif

    // Dequeues everything first and prefetches each op: the worker wrote result/error into
    // the op's first line, so touching them one by one would serialize a cross-core miss
    // per socket; issued together they overlap.
    void drainCompletions() {
        Socket::SendOp *ops[64];
        for (;;) {
            size_t n = 0;
            while (n < 64 && toMain->try_dequeue(ops[n])) {
#if defined(__GNUC__) || defined(__clang__)
                __builtin_prefetch(ops[n]);
#endif
                n++;
            }
            if (n == 0) {
                return;
            }
            for (size_t i = 0; i < n; i++) {
                Socket::sendComplete(ops[i]);
            }
        }
    }

    void onMainWake(uv_async_t *) {
        drainCompletions();
        SendWorker::flush(); // completions may have resubmitted sockets with more queued
    }

    inline void cpuRelax() {
#if defined(_MSC_VER) && defined(_M_X64)
        _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
        __yield();
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
    }


    void workerLoop() {
        Socket::SendOp *op;
#ifdef __linux__
        CPU_ZERO(&allowedAtStart);
        sched_getaffinity(0, sizeof(allowedAtStart), &allowedAtStart);
#endif
        for (;;) {
            if (!toWorker->peek() && spinMicros > 0) {
                // stay hot between ticks: a wake from the JS thread costs it a futex syscall
                auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(spinMicros);
                for (int i = 0; !toWorker->peek(); i++) {
                    cpuRelax();
                    if ((i & 63) == 63 && std::chrono::steady_clock::now() >= deadline) break;
                }
            }
            if (!toWorker->peek()) {
                // Dekker with flush(): either it sees workerSleeping and signals, or we see its op
                workerSleeping.store(true, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (!toWorker->peek()) {
                    workerSema->wait();
                }
                workerSleeping.store(false, std::memory_order_seq_cst);
            }
            bool any = false, wake = false;
            while (toWorker->try_dequeue(op)) {
                Socket::performSend(op);
                toMain->enqueue(op);
                any = true;
                // A clean completion (everything written, no callback) can wait for the loop's
                // next natural wake; anything else, or a socket that closed / queued more
                // meanwhile, must not sit until then.
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (op->result < 0 || (size_t) op->result != op->bytes || op->hasCallback || op->needWake.load(std::memory_order_seq_cst)) {
                    wake = true;
                }
            }
            if (any) {
                // Dekker with beforePoll(): either it sees our completion, or we see loopIdle
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (wake && loopIdle.load(std::memory_order_seq_cst)) {
                    uv_async_send(&mainWake);
                }
#ifdef __linux__
                if (affinityEnabled) {
                    followJsThread();
                }
#endif
            }
        }
    }
}

bool SendWorker::init(uv_loop_t *loop) {
    const char *disabled = getenv("CWS_SEND_THREAD");
    std::string value = disabled ? disabled : "";
    for (char &c : value) c = (char) tolower((unsigned char) c);
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        workerStatus = "disabled by CWS_SEND_THREAD";
        return false;
    }
    const char *affinity = getenv("CWS_SEND_THREAD_AFFINITY");
    if (affinity && *affinity && (*affinity == '0' || *affinity == 'f' || *affinity == 'n' || *affinity == 'o')) {
        affinityEnabled = false; // "0", "false", "no", "off"
    }
    const char *spin = getenv("CWS_SEND_THREAD_SPIN_US");
    if (spin && *spin) {
        spinMicros = atoi(spin);
    }
    toWorker = new moodycamel::ReaderWriterQueue<Socket::SendOp *>(65536);
    toMain = new moodycamel::ReaderWriterQueue<Socket::SendOp *>(65536);
    workerSema = new moodycamel::spsc_sema::LightweightSemaphore();
    uv_async_init(loop, &mainWake, onMainWake);
    uv_unref((uv_handle_t *) &mainWake); // completions alone must not keep the process alive
    std::thread(workerLoop).detach();
    workerActive = true;
    workerStatus = "active";
    return true;
}

bool SendWorker::active() { return workerActive; }
const char *SendWorker::status() { return workerStatus; }

bool SendWorker::submit(void *op) {
    // try_enqueue never allocates; a full queue means "send it yourself this tick".
    if (!toWorker->try_enqueue((Socket::SendOp *) op)) {
        return false;
    }
    signalPending = true;
    return true;
}

void SendWorker::flush() {
    if (!signalPending) {
        return;
    }
    signalPending = false;
#ifdef __linux__
    if (affinityEnabled) {
        jsCpu.store(sched_getcpu(), std::memory_order_relaxed); // vDSO, ~20 ns
    }
#endif
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (workerSleeping.load(std::memory_order_seq_cst)) {
        workerSema->signal();
    }
}

void SendWorker::beforePoll() {
    if (!workerActive) {
        return;
    }
    loopIdle.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (toMain->peek()) {
        drainCompletions();
        flush();
    }
}

void SendWorker::afterPoll() {
    if (!workerActive) {
        return;
    }
    loopIdle.store(false, std::memory_order_seq_cst);
    drainCompletions(); // flushCorked() follows and signals any resubmits
}

void *SendWorker::allocOp() {
    if (freeOps.empty()) {
        return new Socket::SendOp();
    }
    Socket::SendOp *op = freeOps.back();
    freeOps.pop_back();
    return op;
}

void SendWorker::freeOp(void *op) {
    if (freeOps.size() < 1024) {
        freeOps.push_back((Socket::SendOp *) op);
    } else {
        delete [] ((Socket::SendOp *) op)->scratch;
        delete (Socket::SendOp *) op;
    }
}

// ---- Socket side (needs Socket internals) ----

// Deflates a pending message with the given stream and replaces its raw payload with a
// framed compressed message. Shared between the worker (its own compressor) and the
// main-thread materialize path (the hub's).
// `op` non-null: the frame goes into the op's scratch arena (worker thread); null: heap frame
// owned by the message (main-thread materialize). The raw payload is never freed here: it is
// inline in the message's pool block or kept in rawOwned until the main thread releases it.
static void deflateAndFrame(Socket::Queue::Message *m, cWS::zlib::Stream *stream, bool resetAfter, char *buffer, size_t bufferSize, std::string &dynamic, Socket::SendOp *op) {
    size_t compressedLength = m->length;
    // resetAfter == independent message (no context takeover): microdeflate; else the socket's window
    char *deflated = resetAfter ? cWS::zlib::deflateIndependent(stream, (char *) m->data, compressedLength, buffer, bufferSize, dynamic)
                                : cWS::zlib::deflate(stream, (char *) m->data, compressedLength, buffer, bufferSize, dynamic, false);
    const size_t HEADER = cWS::WebSocketProtocol<true, cWS::WebSocket<true>>::LONG_MESSAGE_HEADER;
    size_t need = compressedLength + HEADER;
    char *frame;
    if (op) {
        if (op->scratchUsed + need > op->scratchCap) {
            size_t cap = op->scratchCap ? op->scratchCap * 2 : 64 * 1024;
            while (cap < op->scratchUsed + need) cap *= 2;
            char *grown = new char[cap];
            if (op->scratchUsed) {
                memcpy(grown, op->scratch, op->scratchUsed);
                for (Socket::Queue::Message *x = op->head; x; x = x->nextMessage) { // rebase frames already in the arena
                    if (x->inScratch) x->data = grown + (x->data - op->scratch);
                }
            }
            delete [] op->scratch;
            op->scratch = grown;
            op->scratchCap = cap;
        }
        frame = op->scratch + op->scratchUsed;
    } else {
        frame = new char[need];
    }
    size_t frameLength = cWS::WebSocketProtocol<true, cWS::WebSocket<true>>::formatMessage(frame, deflated, compressedLength, (cWS::OpCode) m->opCode, compressedLength, true);
    if (op) {
        op->scratchUsed += frameLength;
        m->inScratch = true;
    } else {
        m->ownsData = true;
    }
    m->data = frame;
    m->length = frameLength;
    m->compressPending = false;
}

namespace {
    // worker-thread compression state: its own shared compressor and scratch buffers
    cWS::zlib::Stream *workerDeflate = nullptr;
    const size_t WORKER_BUFFER = 300 * 1024;
    char *workerBuffer = nullptr;
    std::string workerDynamic;
}

void Socket::materializeOnMain(Socket *s, Queue::Message *m, cWS::zlib::Stream *stream, bool resetAfter, char *buffer, size_t bufferSize, std::string &dynamic) {
    deflateAndFrame(m, stream, resetAfter, buffer, bufferSize, dynamic, nullptr);
}

// A partially sent frame that lives in the op's scratch would dangle once the op is reused:
// move what is left of it to the heap.
void Socket::unscratch(SendOp *op) {
    for (Queue::Message *m = op->head; m; m = m->nextMessage) {
        if (m->inScratch) {
            char *copy = new char[m->length ? m->length : 1];
            memcpy(copy, m->data, m->length);
            m->data = copy;
            m->ownsData = true;
            m->inScratch = false;
        }
    }
}

void Socket::prepareSend(SendOp *op) {
    if (!workerDeflate) {
        workerDeflate = cWS::zlib::createDeflate(1, 15, 8);
        workerBuffer = new char[WORKER_BUFFER];
    }
    // 1. deflate + frame anything still pending, with this socket's window or the shared one
    cWS::zlib::Stream *stream = op->deflateWindow ? (cWS::zlib::Stream *) op->deflateWindow : workerDeflate;
    op->scratchUsed = 0;
    for (Queue::Message *m = op->head; m; m = m->nextMessage) {
        if (m->compressPending) {
            deflateAndFrame(m, stream, !op->deflateWindow, workerBuffer, WORKER_BUFFER, workerDynamic, op);
        }
    }
    // 2. gather
    op->count = 0;
    op->bytes = 0;
    op->hasCallback = false;
    for (Queue::Message *m = op->head; m; m = m->nextMessage) {
#ifdef _WIN32
        op->bufs[op->count].buf = (CHAR *) m->data;
        op->bufs[op->count].len = (ULONG) m->length;
#else
        op->iov[op->count].iov_base = (void *) m->data;
        op->iov[op->count].iov_len = m->length;
#endif
        op->bytes += m->length;
        op->count++;
        if (m->callback) {
            op->hasCallback = true;
        }
    }
}

void Socket::performSend(SendOp *op) {
    prepareSend(op);
#ifdef _WIN32
    DWORD sent = 0;
    if (WSASend(op->fd, op->bufs, (DWORD) op->count, &sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
        op->result = -1;
        op->error = WSAGetLastError();
    } else {
        op->result = (ssize_t) sent;
        op->error = 0;
    }
#else
    msghdr msg = {};
    msg.msg_iov = op->iov;
    msg.msg_iovlen = op->count;
    op->result = ::sendmsg(op->fd, &msg, MSG_NOSIGNAL);
    op->error = op->result < 0 ? errno : 0;
#endif
}

static bool sendWouldBlock(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

void Socket::sendComplete(SendOp *op) {
    Socket *s = op->socket;
    if (!s) {
        // socket closed while the op was in flight: release its messages, then the fd we kept open
        for (Queue::Message *m = op->head; m;) {
            Queue::Message *next = m->nextMessage;
            if (m->callback) {
                m->callback(nullptr, m->callbackData, true, m->reserved);
            }
            Queue::release(op->nodeData, m);
            m = next;
        }
        if (op->closeFd) {
            op->nodeData->netContext->closeSocket(op->fd);
        }
        if (op->destroyWindow) {
            cWS::zlib::destroy((cWS::zlib::Stream *) op->destroyWindow);
        }
        SendWorker::freeOp(op);
        return;
    }
    s->sendOp = nullptr;

    ssize_t res = op->result;
    if (res < 0) {
        if (sendWouldBlock(op->error)) {
            res = 0;
        } else {
            unscratch(op);
            s->requeueFront(op);
            SendWorker::freeOp(op);
            if (s->endCb) {
                s->endCb(s);
            }
            return;
        }
    }

    std::vector<PendingCallback> callbacks;
    size_t sent = (size_t) res, opBytes = op->bytes;
    while (sent > 0 && op->head) {
        Queue::Message *m = op->head;
        if (sent >= m->length) {
            sent -= m->length;
            op->bytes -= m->length;
            if (m->callback) {
                callbacks.push_back({m->callback, m->callbackData, m->reserved});
            }
            op->head = m->nextMessage;
            Queue::release(op->nodeData, m);
        } else {
            m->length -= sent;
            m->data += sent;
            op->bytes -= sent;
            sent = 0;
        }
    }
    if (!op->head) {
        op->tail = nullptr;
    }
    bool complete = (size_t) res == opBytes;
    unscratch(op);
    s->requeueFront(op);
    SendWorker::freeOp(op);

    if (!s->messageQueue.empty()) {
        if (complete) {
            s->submitToWorker();                  // more arrived while in flight
        } else if ((s->getPoll() & UV_WRITABLE) == 0) {
            s->setPoll(s->getPoll() | UV_WRITABLE); // kernel buffer full: the drain loop resumes when writable
            s->changePoll(s);
        }
    }
    for (PendingCallback &c : callbacks) {
        c.callback(s, c.callbackData, false, c.reserved);
        if (s->isClosed()) {
            break;
        }
    }
}

}
