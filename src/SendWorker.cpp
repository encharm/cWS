#include "SendWorker.h"
#include "Socket.h"
#include "Zlib.h"
#include "cWS.h"
#include "readerwriterqueue.h"
#include <thread>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <string>

namespace cS {

namespace {
    bool workerActive = false;
    const char *workerStatus = "not initialised";
    moodycamel::BlockingReaderWriterQueue<Socket::SendOp *> *toWorker = nullptr;
    moodycamel::ReaderWriterQueue<Socket::SendOp *> *toMain = nullptr;
    uv_async_t mainWake;

    void onMainWake(uv_async_t *) {
        Socket::SendOp *op;
        while (toMain->try_dequeue(op)) {
            Socket::sendComplete(op);
        }
    }

    void workerLoop() {
        Socket::SendOp *op;
        for (;;) {
            toWorker->wait_dequeue(op);
            Socket::performSend(op);
            toMain->enqueue(op);
            uv_async_send(&mainWake);
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
    toWorker = new moodycamel::BlockingReaderWriterQueue<Socket::SendOp *>(4096);
    toMain = new moodycamel::ReaderWriterQueue<Socket::SendOp *>(4096);
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
    return toWorker->try_enqueue((Socket::SendOp *) op);
}

// ---- Socket side (needs Socket internals) ----

// Deflates a pending message with the given stream and replaces its raw payload with a
// framed compressed message. Shared between the worker (its own compressor) and the
// main-thread materialize path (the hub's).
static void deflateAndFrame(Socket::Queue::Message *m, cWS::zlib::Stream *stream, bool resetAfter, char *buffer, size_t bufferSize, std::string &dynamic) {
    size_t compressedLength = m->length;
    // resetAfter == independent message (no context takeover): microdeflate; else the socket's window
    char *deflated = resetAfter ? cWS::zlib::deflateIndependent(stream, (char *) m->data, compressedLength, buffer, bufferSize, dynamic)
                                : cWS::zlib::deflate(stream, (char *) m->data, compressedLength, buffer, bufferSize, dynamic, false);
    const size_t HEADER = cWS::WebSocketProtocol<true, cWS::WebSocket<true>>::LONG_MESSAGE_HEADER;
    char *frame = new char[compressedLength + HEADER];
    size_t frameLength = cWS::WebSocketProtocol<true, cWS::WebSocket<true>>::formatMessage(frame, deflated, compressedLength, (cWS::OpCode) m->opCode, compressedLength, true);
    delete [] (char *) m->data;
    m->data = frame;
    m->length = frameLength;
    m->ownsData = true;
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
    deflateAndFrame(m, stream, resetAfter, buffer, bufferSize, dynamic);
}

void Socket::performSend(SendOp *op) {
    if (!workerDeflate) {
        workerDeflate = cWS::zlib::createDeflate(1, 15, 8);
        workerBuffer = new char[WORKER_BUFFER];
    }
    // 1. deflate + frame anything still pending, with this socket's window or the shared one
    cWS::zlib::Stream *stream = op->deflateWindow ? (cWS::zlib::Stream *) op->deflateWindow : workerDeflate;
    for (Queue::Message *m = op->head; m; m = m->nextMessage) {
        if (m->compressPending) {
            deflateAndFrame(m, stream, !op->deflateWindow, workerBuffer, WORKER_BUFFER, workerDynamic);
        }
    }
    // 2. gather
    op->count = 0;
    op->bytes = 0;
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
    }
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
        delete op;
        return;
    }
    s->sendOp = nullptr;

    ssize_t res = op->result;
    if (res < 0) {
        if (sendWouldBlock(op->error)) {
            res = 0;
        } else {
            s->requeueFront(op);
            delete op;
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
    s->requeueFront(op);
    delete op;

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
