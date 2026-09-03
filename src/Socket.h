#ifndef SOCKET_CWS_H
#define SOCKET_CWS_H

#include "Networking.h"
#include "SendWorker.h"
#include "Zlib.h"
#include <vector>
#include <atomic>
#ifndef _WIN32
#include <sys/uio.h>
#endif

namespace cS {

struct TransferData {
    // Connection state
    uv_os_sock_t fd;
    SSL *ssl;

    // Poll state
    void (*pollCb)(Poll *, int, int);
    int pollEvents;

    // User state
    void *userData;

    // Destination
    NodeData *destination;
    void (*transferCb)(Poll *);
};

// perfectly 64 bytes (4 + 60) (only for EPoll and if packed)
struct WIN32_EXPORT Socket : Poll {
    struct {
        int poll : 4;
        int shuttingDown : 4;
    } state = {0, false};
protected:
    SSL *ssl;
    void *user = nullptr;
    NodeData *nodeData;

    // Corking (see NodeData::CorkState). Only WebSockets opt in; HttpSocket
    // objects are deleted synchronously in Hub::upgrade so they must never
    // sit in the pending list. Plain TCP only, SSL keeps the immediate path.
    bool corkable = false;
    bool corkPending = false;

public:
    // this is not needed by HttpSocket!
    struct Queue {
        struct Message {
            const char *data;
            size_t length;
            Message *nextMessage = nullptr;
            void (*callback)(void *socket, void *data, bool cancelled, void *reserved) = nullptr;
            void *callbackData = nullptr, *reserved = nullptr;
            // >= 0: block came from NodeData's small-block pool (index for freeSmallMemoryBlock);
            // -1: heap (new char[]). Messages are allocated as raw char arrays, so every
            // allocation site must set this explicitly (the initializers above never run).
            int poolIndex = -1;
            // data lives in its own new[] buffer (not inside this block); freed with delete[].
            bool ownsData = false;
            // data is a raw, unframed payload still to be deflated and framed (by the send
            // worker, or by materializeCb if a main-thread write path reaches it first).
            bool compressPending = false;
            // A slab (pool block of NodeData::slabSize with inline data) that later corked sends
            // may still append frames to while it sits at the queue tail. Never true for a
            // message with a callback or borrowed data.
            bool run = false;
            // data points into the sending op's scratch arena (a compressed frame built by the
            // worker); freed with the op, never by release(). A partially sent scratch frame
            // is copied to the heap by sendComplete before it is requeued.
            bool inScratch = false;
            // A heap raw payload (compressPending message too large for a pool block) kept
            // until release(), so the worker never frees main-thread memory.
            const char *rawOwned = nullptr;
            unsigned char opCode = 0;
        };

        Message *head = nullptr, *tail = nullptr;
        size_t totalLength = 0;

        static void release(NodeData *nodeData, Message *message) {
            if (message->ownsData) {
                delete [] (char *) message->data;
            }
            if (message->rawOwned) {
                delete [] (char *) message->rawOwned;
            }
            if (message->poolIndex >= 0) {
                nodeData->freeSmallMemoryBlock((char *) message, message->poolIndex);
            } else {
                delete [] (char *) message;
            }
        }

        void pop(NodeData *nodeData)
        {
            Message *nextMessage;
            if ((nextMessage = head->nextMessage)) {
                totalLength -= head->length;
                release(nodeData, head);
                head = nextMessage;
            } else {
                release(nodeData, head);
                head = tail = nullptr;
                totalLength = 0;
            }
        }

        bool empty() {return head == nullptr;}
        Message *front() {return head;}

        void push(Message *message)
        {
            message->nextMessage = nullptr;
            if (tail) {
                tail->nextMessage = message;
                tail = message;
            } else {
                head = message;
                tail = message;
            }
            totalLength += message->length;
        }
    } messageQueue;

    // One in-flight worker-thread send per socket (see SendWorker.h). The frames it
    // covers are moved out of messageQueue into the op, so nothing the worker reads
    // can be freed underneath it; the main thread takes them back on completion.
    // `socket` is nulled if the socket closes first, and the fd is then closed by the
    // completion (never while the worker may still be inside a send on it).
    struct SendOp {
        Socket *socket;
        NodeData *nodeData;
        Queue::Message *head, *tail;
        size_t bytes;
        int count;
        uv_os_sock_t fd;
        ssize_t result;
        int error;
        bool closeFd;
        // Set by the worker while gathering: some message carries a callback (needs main promptly).
        bool hasCallback;
        // Set by the main thread while the op is in flight when its completion must be handled
        // promptly even if the loop is idle: the socket closed (fd to close) or more messages
        // queued behind the op. Read by the worker after posting the completion (Dekker with
        // the main thread's hook drains: a set the worker misses is followed by a main-thread
        // drain before the loop sleeps, because the setter runs on the main thread).
        std::atomic<bool> needWake;
        void *deflateWindow;   // this socket's sliding window (nullptr: worker's shared compressor)
        void *destroyWindow;   // set when the socket closed mid-flight: completion destroys it
        // Worker-owned arena for the compressed frames of this op (grows in place, pointers
        // rebased); reused with the op, freed only when the op itself is deleted.
        char *scratch;
        size_t scratchCap, scratchUsed;
#ifdef _WIN32
        WSABUF bufs[512];
#else
        struct iovec iov[512];
#endif
    };
    static void prepareSend(SendOp *op);   // worker thread: compress pending, build iov
    static void performSend(SendOp *op);   // worker thread
    static void sendComplete(SendOp *op);    // main thread
    static void materializeOnMain(Socket *s, Queue::Message *m, cWS::zlib::Stream *stream, bool resetAfter, char *buffer, size_t bufferSize, std::string &dynamic);
    static void unscratch(SendOp *op);   // main thread: copy partially sent scratch frames to the heap before requeue
protected:
    SendOp *sendOp = nullptr;
    // Set by setState<STATE>(): STATE::onEnd, so the completion path can end a socket on a hard error.
    void (*endCb)(Socket *) = nullptr;
    // Per-socket deflate window handed to worker ops (WebSocket sets it); once an op has
    // been orphaned by close, the op owns the window and the socket must not destroy it.
    void *workerDeflateWindow = nullptr;
    bool workerOwnsWindow = false;
    // Deflates + frames a compressPending message on the main thread (set by WebSocket).
    void (*materializeCb)(Socket *, Queue::Message *) = nullptr;

    void materializePending() {
        for (Queue::Message *m = messageQueue.front(); m; m = m->nextMessage) {
            if (m->compressPending && materializeCb) {
                materializeCb(this, m);
            }
        }
    }

    bool sendBusy() {
        return sendOp != nullptr;
    }

    int getPoll() {
        return state.poll;
    }

    int setPoll(int poll) {
        state.poll = poll;
        return poll;
    }

    void setShuttingDown(bool shuttingDown) {
        state.shuttingDown = shuttingDown;
    }

    void transfer(NodeData *nodeData, void (*cb)(Poll *)) {
        flushCorkedOnClose();
        // userData is invalid from now on till onTransfer
        setUserData(new TransferData({getFd(), ssl, getCb(), getPoll(), getUserData(), nodeData, cb}));
        stop(this->nodeData->loop);
        close(this->nodeData->loop, [](Poll *p) {
            Socket *s = (Socket *) p;
            TransferData *transferData = (TransferData *) s->getUserData();

            transferData->destination->asyncMutex->lock();
            bool wasEmpty = transferData->destination->transferQueue.empty();
            transferData->destination->transferQueue.push_back(s);
            transferData->destination->asyncMutex->unlock();

            if (wasEmpty) {
                transferData->destination->async->send();
            }
        });
    }

    void changePoll(Socket *socket) {
        if (!threadSafeChange(nodeData->loop, this, socket->getPoll())) {
            if (socket->nodeData->tid != pthread_self()) {
                socket->nodeData->asyncMutex->lock();
                socket->nodeData->changePollQueue.push_back(socket);
                socket->nodeData->asyncMutex->unlock();
                socket->nodeData->async->send();
            } else {
                change(socket->nodeData->loop, socket, socket->getPoll());
            }
        }
    }

    // clears user data!
    template <void onTimeout(Socket *)>
    void startTimeout(int timeoutMs = 15000) {
        Timer *timer = new Timer(nodeData->loop);
        timer->setData(this);
        timer->start([](Timer *timer) {
            Socket *s = (Socket *) timer->getData();
            s->cancelTimeout();
            onTimeout(s);
        }, timeoutMs, 0);

        user = timer;
    }

    void cancelTimeout() {
        Timer *timer = (Timer *) getUserData();
        if (timer) {
            timer->stop();
            timer->close();
            user = nullptr;
        }
    }

    template <class STATE>
    static void sslIoHandler(Poll *p, int status, int events) {
        Socket *socket = (Socket *) p;

        if (status < 0) {
            STATE::onEnd((Socket *) p);
            return;
        }

        if (!socket->messageQueue.empty() && ((events & UV_WRITABLE) || SSL_want(socket->ssl) == SSL_READING)) {
            socket->cork(true);
            while (true) {
                Queue::Message *messagePtr = socket->messageQueue.front();
                int sent = SSL_write(socket->ssl, messagePtr->data, (int) messagePtr->length);
                if (sent == (ssize_t) messagePtr->length) {
                    // Pop before the callback: it may close/terminate this socket,
                    // which frees the queue (use-after-free otherwise).
                    auto callback = messagePtr->callback;
                    void *callbackData = messagePtr->callbackData, *reserved = messagePtr->reserved;
                    socket->messageQueue.pop(socket->nodeData);
                    if (callback) {
                        callback(p, callbackData, false, reserved);
                        if (socket->isClosed()) {
                            return;
                        }
                    }
                    if (socket->messageQueue.empty()) {
                        if ((socket->state.poll & UV_WRITABLE) && SSL_want(socket->ssl) != SSL_WRITING) {
                            socket->change(socket->nodeData->loop, socket, socket->setPoll(UV_READABLE));
                        }
                        break;
                    }
                } else if (sent <= 0) {
                    switch (SSL_get_error(socket->ssl, sent)) {
                    case SSL_ERROR_WANT_READ:
                        break;
                    case SSL_ERROR_WANT_WRITE:
                        if ((socket->getPoll() & UV_WRITABLE) == 0) {
                            socket->change(socket->nodeData->loop, socket, socket->setPoll(socket->getPoll() | UV_WRITABLE));
                        }
                        break;
                    default:
                        STATE::onEnd((Socket *) p);
                        return;
                    }
                    break;
                }
            }
            socket->cork(false);
        }

        if (events & UV_READABLE) {
            do {
                int length = SSL_read(socket->ssl, socket->nodeData->recvBuffer, socket->nodeData->recvLength);
                if (length <= 0) {
                    switch (SSL_get_error(socket->ssl, length)) {
                    case SSL_ERROR_WANT_READ:
                        break;
                    case SSL_ERROR_WANT_WRITE:
                        if ((socket->getPoll() & UV_WRITABLE) == 0) {
                            socket->change(socket->nodeData->loop, socket, socket->setPoll(socket->getPoll() | UV_WRITABLE));
                        }
                        break;
                    default:
                        STATE::onEnd((Socket *) p);
                        return;
                    }
                    break;
                } else {
                    // Warning: onData can delete the socket! Happens when HttpSocket upgrades
                    socket = STATE::onData((Socket *) p, socket->nodeData->recvBuffer, length);
                    if (socket->isClosed() || socket->isShuttingDown()) {
                        return;
                    }
                }
            } while (SSL_pending(socket->ssl));
        }
    }

    template <class STATE>
    static void ioHandler(Poll *p, int status, int events) {
        Socket *socket = (Socket *) p;
        NodeData *nodeData = socket->nodeData;
        Context *netContext = nodeData->netContext;

        if (status < 0) {
            STATE::onEnd((Socket *) p);
            return;
        }

        if (events & UV_WRITABLE) {
            // A socket whose kernel buffer was full became writable again. With the worker on,
            // hand it the queue: a slow client is a steady state, and its drains must not cost
            // the JS thread a syscall per message. The completion re-arms UV_WRITABLE if the
            // write is short again. Without the worker: one gathered write per round, no
            // cork toggles (they were two setsockopt calls per drain).
            if (!socket->messageQueue.empty() && !socket->sendBusy()) {
                if (SendWorker::active() && !socket->ssl) {
                    socket->change(socket->nodeData->loop, socket, socket->setPoll(UV_READABLE));
                    if (socket->submitToWorker()) {
                        return;
                    }
                    socket->setPoll(socket->getPoll() | UV_WRITABLE); // worker queue full: drain here
                    socket->changePoll(socket);
                }
                socket->materializePending();
                std::vector<PendingCallback> callbacks;
                bool stalled = false;
                while (!socket->messageQueue.empty() && !stalled) {
                    ssize_t sent = socket->writeQueueGathered();
                    if (sent < 0) {
                        if (!netContext->wouldBlock()) {
                            STATE::onEnd((Socket *) p);
                            return;
                        }
                        stalled = true;
                    } else {
                        stalled = socket->consumeSent(sent, &callbacks);
                    }
                }
                if (socket->messageQueue.empty()) {
                    socket->change(socket->nodeData->loop, socket, socket->setPoll(UV_READABLE));
                }
                for (PendingCallback &c : callbacks) {
                    c.callback(p, c.callbackData, false, c.reserved);
                    if (socket->isClosed()) {
                        return;
                    }
                }
            }
        }

        if (events & UV_READABLE) {
            int length = (int) recv(socket->getFd(), nodeData->recvBuffer, nodeData->recvLength, 0);
            if (length > 0) {
                STATE::onData((Socket *) p, nodeData->recvBuffer, length);
            } else if (length <= 0 || (length == SOCKET_ERROR && !netContext->wouldBlock())) {
                STATE::onEnd((Socket *) p);
            }
        }

    }

    template<class STATE>
    void setState() {
        endCb = STATE::onEnd;
        if (ssl) {
            setCb(sslIoHandler<STATE>);
        } else {
            setCb(ioHandler<STATE>);
        }
    }

    bool hasEmptyQueue() {
        return messageQueue.empty();
    }

    void enqueue(Queue::Message *message) {
        messageQueue.push(message);
    }

    // ---- Slab messages (corked path) ----------------------------------------
    static size_t slabCapacity() {
        return cS::NodeData::slabSize - sizeof(Queue::Message);
    }
    // Bytes still free after the written frames of a slab (data may have advanced on a partial write).
    static size_t slabSpaceLeft(Queue::Message *slab) {
        const char *base = ((const char *) slab) + sizeof(Queue::Message);
        return (size_t) ((base + slabCapacity()) - (slab->data + slab->length));
    }
    // The slab at the queue tail if `bytes` fit, else a fresh one appended to the queue.
    // Only the main thread touches the tail, and a message in a worker op is never the
    // tail (submitToWorker unlinks it), so appending never races the worker.
    Queue::Message *slabWithSpace(size_t bytes) {
        Queue::Message *tail = messageQueue.tail;
        if (tail && tail->run && slabSpaceLeft(tail) >= bytes) {
            return tail;
        }
        int memoryIndex = nodeData->getMemoryBlockIndex(cS::NodeData::slabSize);
        Queue::Message *slab = (Queue::Message *) nodeData->getSmallMemoryBlock(memoryIndex);
        slab->data = ((char *) slab) + sizeof(Queue::Message);
        slab->length = 0;
        slab->callback = nullptr;
        slab->callbackData = nullptr;
        slab->reserved = nullptr;
        slab->poolIndex = memoryIndex;
        slab->ownsData = false;
        slab->compressPending = false;
        slab->run = true;
        slab->inScratch = false;
        slab->rawOwned = nullptr;
        slab->opCode = 0;
        enqueue(slab);
        return slab;
    }

    Queue::Message *allocMessage(size_t length, const char *data = 0) {
        Queue::Message *messagePtr = (Queue::Message *) new char[sizeof(Queue::Message) + length];
        messagePtr->length = length;
        messagePtr->data = ((char *) messagePtr) + sizeof(Queue::Message);
        messagePtr->nextMessage = nullptr;
        messagePtr->callback = nullptr;
        messagePtr->callbackData = nullptr;
        messagePtr->reserved = nullptr;
        messagePtr->poolIndex = -1;
        messagePtr->ownsData = false;
        messagePtr->compressPending = false;
        messagePtr->run = false;
        messagePtr->inScratch = false;
        messagePtr->rawOwned = nullptr;
        messagePtr->opCode = 0;

        if (data) {
            memcpy((char *) messagePtr->data, data, messagePtr->length);
        }

        return messagePtr;
    }

    void freeMessage(Queue::Message *message) {
        delete [] (char *) message;
    }

    bool write(Queue::Message *message, bool &wasTransferred) {
        ssize_t sent = 0;
        if (sendBusy()) {
            // a worker send is in flight: anything written now must follow it, and the
            // completion must resubmit promptly even if the loop is idle by then
            messageQueue.push(message);
            sendOp->needWake.store(true, std::memory_order_seq_cst);
            wasTransferred = true;
            return true;
        }
        if (messageQueue.empty()) {

            if (ssl) {
                sent = SSL_write(ssl, message->data, (int) message->length);
                if (sent == (ssize_t) message->length) {
                    wasTransferred = false;
                    return true;
                } else if (sent < 0) {
                    switch (SSL_get_error(ssl, (int) sent)) {
                    case SSL_ERROR_WANT_READ:
                        break;
                    case SSL_ERROR_WANT_WRITE:
                        if ((getPoll() & UV_WRITABLE) == 0) {
                            setPoll(getPoll() | UV_WRITABLE);
                            changePoll(this);
                        }
                        break;
                    default:
                        return false;
                    }
                }
            } else {
                sent = ::send(getFd(), message->data, message->length, MSG_NOSIGNAL);
                if (sent == (ssize_t) message->length) {
                    wasTransferred = false;
                    return true;
                } else if (sent == SOCKET_ERROR) {
                    if (!nodeData->netContext->wouldBlock()) {
                        return false;
                    }
                } else {
                    message->length -= sent;
                    message->data += sent;
                }

                if ((getPoll() & UV_WRITABLE) == 0) {
                    setPoll(getPoll() | UV_WRITABLE);
                    changePoll(this);
                }
            }
        }
        messageQueue.push(message);
        wasTransferred = true;
        return true;
    }

    template <class T, class D>
    void sendTransformed(const char *message, size_t length, void(*callback)(void *socket, void *data, bool cancelled, void *reserved), void *callbackData, D transformData) {
        size_t estimatedLength = T::estimate(message, length) + sizeof(Queue::Message);

        if (corkActive()) {
            // Frame now, write later: everything sent to this socket during the current
            // loop iteration goes out in one gathered write (uncork()). Plain frames are
            // appended to the slab at the queue tail, so a whole tick is usually one
            // message: no allocation or list node per send, one iovec on the worker, one
            // pop on completion. Frames with a callback keep their own message (callbacks
            // are per message); frames larger than a slab go to the heap.
            size_t frameLength = estimatedLength - sizeof(Queue::Message);
            Queue::Message *messagePtr;
            if (!callback && frameLength <= slabCapacity()) {
                messagePtr = slabWithSpace(frameLength);
                size_t n = T::transform(message, (char *) messagePtr->data + messagePtr->length, length, transformData);
                messagePtr->length += n;
                messageQueue.totalLength += n;
            } else {
                if (estimatedLength <= cS::NodeData::preAllocMaxSize) {
                    int memoryIndex = nodeData->getMemoryBlockIndex((int) estimatedLength);
                    messagePtr = (Queue::Message *) nodeData->getSmallMemoryBlock(memoryIndex);
                    messagePtr->data = ((char *) messagePtr) + sizeof(Queue::Message);
                    messagePtr->poolIndex = memoryIndex;
                    messagePtr->ownsData = false;
                    messagePtr->compressPending = false;
                    messagePtr->run = false;
                    messagePtr->inScratch = false;
                    messagePtr->rawOwned = nullptr;
                    messagePtr->opCode = 0;
                } else {
                    messagePtr = allocMessage(estimatedLength - sizeof(Queue::Message));
                }
                messagePtr->length = T::transform(message, (char *) messagePtr->data, length, transformData);
                messagePtr->nextMessage = nullptr;
                messagePtr->callback = callback;
                messagePtr->callbackData = callbackData;
                messagePtr->reserved = nullptr;
                enqueue(messagePtr);
            }
            if (!corkPending) {
                corkPending = true;
                nodeData->corkState->pending.push_back(this);
            }
            return;
        }

        if (hasEmptyQueue()) {
            if (estimatedLength <= cS::NodeData::preAllocMaxSize) {
                int memoryLength = (int) estimatedLength;
                int memoryIndex = nodeData->getMemoryBlockIndex(memoryLength);

                Queue::Message *messagePtr = (Queue::Message *) nodeData->getSmallMemoryBlock(memoryIndex);
                messagePtr->data = ((char *) messagePtr) + sizeof(Queue::Message);
                messagePtr->length = T::transform(message, (char *) messagePtr->data, length, transformData);
                messagePtr->nextMessage = nullptr;
                messagePtr->reserved = nullptr;
                messagePtr->poolIndex = memoryIndex;
                messagePtr->ownsData = false;
                messagePtr->compressPending = false;
                messagePtr->run = false;
                messagePtr->inScratch = false;
                messagePtr->rawOwned = nullptr;
                messagePtr->opCode = 0;

                bool wasTransferred;
                if (write(messagePtr, wasTransferred)) {
                    if (!wasTransferred) {
                        nodeData->freeSmallMemoryBlock((char *) messagePtr, memoryIndex);
                        if (callback) {
                            callback(this, callbackData, false, nullptr);
                        }
                    } else {
                        messagePtr->callback = callback;
                        messagePtr->callbackData = callbackData;
                    }
                } else {
                    nodeData->freeSmallMemoryBlock((char *) messagePtr, memoryIndex);
                    if (callback) {
                        callback(this, callbackData, true, nullptr);
                    }
                }
            } else {
                Queue::Message *messagePtr = allocMessage(estimatedLength - sizeof(Queue::Message));
                messagePtr->length = T::transform(message, (char *) messagePtr->data, length, transformData);

                bool wasTransferred;
                if (write(messagePtr, wasTransferred)) {
                    if (!wasTransferred) {
                        freeMessage(messagePtr);
                        if (callback) {
                            callback(this, callbackData, false, nullptr);
                        }
                    } else {
                        messagePtr->callback = callback;
                        messagePtr->callbackData = callbackData;
                    }
                } else {
                    freeMessage(messagePtr);
                    if (callback) {
                        callback(this, callbackData, true, nullptr);
                    }
                }
            }
        } else {
            Queue::Message *messagePtr = allocMessage(estimatedLength - sizeof(Queue::Message));
            messagePtr->length = T::transform(message, (char *) messagePtr->data, length, transformData);
            messagePtr->callback = callback;
            messagePtr->callbackData = callbackData;
            enqueue(messagePtr);
        }
    }

public:
    auto getBufferedAmount() {
        return messageQueue.totalLength + (sendOp ? sendOp->bytes : 0);
    }

    Socket(NodeData *nodeData, Loop *loop, uv_os_sock_t fd, SSL *ssl) : Poll(loop, fd), ssl(ssl), nodeData(nodeData) {
        if (ssl) {
            // OpenSSL treats SOCKETs as int
            SSL_set_fd(ssl, (int) fd);
            SSL_set_mode(ssl, SSL_MODE_RELEASE_BUFFERS);
        }
    }

    NodeData *getNodeData() {
        return nodeData;
    }

    Poll *next = nullptr, *prev = nullptr;

    void *getUserData() {
        return user;
    }

    void setUserData(void *user) {
        this->user = user;
    }

    struct Address {
        unsigned int port;
        const char *address;
        const char *family;
    };

    Address getAddress();

    void setNoDelay(int enable) {
        setsockopt(getFd(), IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(int));
    }

    void cork(int enable) {
#if defined(TCP_CORK)
        // Linux & SmartOS have proper TCP_CORK
        setsockopt(getFd(), IPPROTO_TCP, TCP_CORK, &enable, sizeof(int));
#elif defined(TCP_NOPUSH)
        // Mac OS X & FreeBSD have TCP_NOPUSH
        setsockopt(getFd(), IPPROTO_TCP, TCP_NOPUSH, &enable, sizeof(int));
        if (!enable) {
            // Tested on OS X, FreeBSD situation is unclear
            ::send(getFd(), "", 0, MSG_NOSIGNAL);
        }
#endif
    }

    void shutdown() {
        if (ssl) {
            //todo: poll in/out - have the io_cb recall shutdown if failed
            SSL_shutdown(ssl);
        } else {
            ::shutdown(getFd(), SHUT_WR);
        }
    }

    // ---- Write corking ------------------------------------------------------

    bool corkActive() {
        return corkable && !ssl && nodeData->corkState && nodeData->corkState->enabled;
    }

    void setCorkable(bool enable) {
        corkable = enable;
    }

    void unregisterCork() {
        if (!corkPending) {
            return;
        }
        corkPending = false;
        std::vector<Poll *> &pending = nodeData->corkState->pending;
        for (size_t i = 0; i < pending.size(); i++) {
            if (pending[i] == this) {
                pending[i] = pending.back();
                pending.pop_back();
                break;
            }
        }
    }

    // One gathered write of (up to MAX_IOV) messages from the queue head.
    // Returns bytes written or -1 with errno/WSAGetLastError set. Never pops.
    ssize_t writeQueueGathered() {
        static const int MAX_IOV = 64;
#ifdef _WIN32
        WSABUF bufs[MAX_IOV];
        DWORD n = 0;
        for (Queue::Message *m = messageQueue.front(); m && n < MAX_IOV; m = m->nextMessage) {
            bufs[n].buf = (CHAR *) m->data;
            bufs[n].len = (ULONG) m->length;
            n++;
        }
        DWORD sent = 0;
        if (WSASend(getFd(), bufs, n, &sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
            return -1;
        }
        return (ssize_t) sent;
#else
        iovec iov[MAX_IOV];
        int n = 0;
        for (Queue::Message *m = messageQueue.front(); m && n < MAX_IOV; m = m->nextMessage) {
            iov[n].iov_base = (void *) m->data;
            iov[n].iov_len = m->length;
            n++;
        }
        msghdr msg = {};
        msg.msg_iov = iov;
        msg.msg_iovlen = n;
        return ::sendmsg(getFd(), &msg, MSG_NOSIGNAL);
#endif
    }

    // Consumes `sent` bytes from the queue head. Fully written messages are
    // popped (their callbacks appended to `callbacks` if given); a partially
    // written head is advanced in place. Returns true if the write was partial.
    struct PendingCallback {
        void (*callback)(void *socket, void *data, bool cancelled, void *reserved);
        void *callbackData, *reserved;
    };

    bool consumeSent(ssize_t sent, std::vector<PendingCallback> *callbacks) {
        while (sent > 0 && !messageQueue.empty()) {
            Queue::Message *m = messageQueue.front();
            if ((size_t) sent >= m->length) {
                sent -= m->length;
                if (callbacks && m->callback) {
                    callbacks->push_back({m->callback, m->callbackData, m->reserved});
                }
                messageQueue.pop(nodeData);
            } else {
                m->length -= sent;
                m->data += sent;
                messageQueue.totalLength -= sent;
                return true;
            }
        }
        return false;
    }

    // Flush this socket's corked queue. Called from the loop's prepare/check
    // hooks via flushCorked(). Whatever cannot be written now is left to the
    // regular UV_WRITABLE drain loop, which also surfaces hard errors (onEnd).
    void uncork() {
        corkPending = false;
        if (isClosed() || messageQueue.empty()) {
            return;
        }

        if (!ssl && SendWorker::active()) {
            if (sendBusy()) {
                sendOp->needWake.store(true, std::memory_order_seq_cst);
                return; // the completion submits what has queued up meanwhile
            }
            if (getPoll() & UV_WRITABLE) {
                change(nodeData->loop, this, setPoll(UV_READABLE));
            }
            if (submitToWorker()) {
                return;
            }
            // worker queue full: fall through and send synchronously this tick
        }
        materializePending();

        std::vector<PendingCallback> callbacks;
        bool stalled = false;
        while (!messageQueue.empty() && !stalled) {
            ssize_t sent = writeQueueGathered();
            if (sent < 0) {
                stalled = true; // would block, or hard error: drain loop reports it
            } else {
                stalled = consumeSent(sent, &callbacks);
            }
        }

        if (!messageQueue.empty()) {
            if ((getPoll() & UV_WRITABLE) == 0) {
                setPoll(getPoll() | UV_WRITABLE);
                changePoll(this);
            }
        } else if (getPoll() & UV_WRITABLE) {
            change(nodeData->loop, this, setPoll(UV_READABLE));
        }

        for (PendingCallback &c : callbacks) {
            c.callback(this, c.callbackData, false, c.reserved);
            if (isClosed()) {
                break; // callback closed us; nothing further may touch the fd
            }
        }
    }

    // Best effort on close/terminate/transfer: push what was corked during this
    // iteration to the kernel so send() immediately followed by terminate()
    // still delivers. Callbacks are not invoked; the caller drains the queue.
    void flushCorkedOnClose() {
        if (!corkPending) {
            return;
        }
        unregisterCork();
        if (isClosed() || sendBusy()) {
            return;
        }
        materializePending();
        while (!messageQueue.empty()) {
            ssize_t sent = writeQueueGathered();
            if (sent <= 0 || consumeSent(sent, nullptr)) {
                break;
            }
        }
    }

    // Loop hook: flush every socket that corked writes since the last hook.
    static void flushCorked(NodeData *nodeData) {
        if (nodeData->corkState && !nodeData->corkState->pending.empty()) {
            // Swap out the list: callbacks run during uncork() may cork new writes
            // (picked up by the next hook) or close other sockets (their deletion
            // is deferred to libuv's close phase, so pointers stay valid here).
            std::vector<Poll *> &batch = nodeData->corkState->flushing; // kept: no reallocation per tick
            batch.clear();
            batch.swap(nodeData->corkState->pending);
            for (Poll *p : batch) {
                ((Socket *) p)->uncork();
            }
            batch.clear();
        }
        if (SendWorker::active()) {
            SendWorker::flush(); // one wake for every op submitted this hook
        }
    }

    // Moves up to 512 queued frames into an op and hands it to the worker thread.
    bool submitToWorker() {
        if (sendOp || messageQueue.empty() || isClosed()) {
            return true;
        }
        SendOp *op = (SendOp *) SendWorker::allocOp();
        op->socket = this;
        op->nodeData = nodeData;
        op->fd = getFd();
        op->head = messageQueue.head;
        op->bytes = 0;
        op->count = 0;
        op->result = 0;
        op->error = 0;
        op->closeFd = false;
        op->hasCallback = false;
        op->needWake.store(false, std::memory_order_relaxed);
        op->deflateWindow = workerDeflateWindow;
        op->destroyWindow = nullptr;
        Queue::Message *m = messageQueue.head, *last = nullptr;
        while (m && op->count < 512) {
            // iov entries are filled by the worker once pending messages are compressed
            op->bytes += m->length;
            op->count++;
            last = m;
            m = m->nextMessage;
        }
        op->tail = last;
        last->nextMessage = nullptr;
        messageQueue.head = m;
        if (!m) {
            messageQueue.tail = nullptr;
            messageQueue.totalLength = 0;
        } else {
            messageQueue.totalLength -= op->bytes;
        }
        if (!SendWorker::submit(op)) {
            requeueFront(op);
            SendWorker::freeOp(op);
            return false;
        }
        sendOp = op;
        return true;
    }

    // Queues a raw payload to be deflated and framed on the send worker (WebSocket::send
    // uses this for compressed messages when the worker is active). The payload is copied
    // because callers may reuse their buffer as soon as send() returns.
    void enqueueCompressPending(const char *payload, size_t length, unsigned char opCode,
                                void(*callback)(void *socket, void *data, bool cancelled, void *reserved), void *callbackData) {
        // The raw payload lives inline in a pool block when it fits (freed by the main thread
        // with the message), else on the heap, kept in rawOwned until release().
        size_t inlineSize = sizeof(Queue::Message) + (length ? length : 1);
        Queue::Message *messagePtr;
        if (inlineSize <= (size_t) cS::NodeData::preAllocMaxSize) {
            int memoryIndex = nodeData->getMemoryBlockIndex((int) inlineSize);
            messagePtr = (Queue::Message *) nodeData->getSmallMemoryBlock(memoryIndex);
            messagePtr->data = ((char *) messagePtr) + sizeof(Queue::Message);
            messagePtr->poolIndex = memoryIndex;
            messagePtr->rawOwned = nullptr;
        } else {
            int memoryIndex = nodeData->getMemoryBlockIndex(sizeof(Queue::Message));
            messagePtr = (Queue::Message *) nodeData->getSmallMemoryBlock(memoryIndex);
            char *raw = new char[length];
            messagePtr->data = raw;
            messagePtr->poolIndex = memoryIndex;
            messagePtr->rawOwned = raw;
        }
        memcpy((char *) messagePtr->data, payload, length);
        messagePtr->length = length;
        messagePtr->nextMessage = nullptr;
        messagePtr->callback = callback;
        messagePtr->callbackData = callbackData;
        messagePtr->reserved = nullptr;
        messagePtr->ownsData = false;
        messagePtr->compressPending = true;
        messagePtr->run = false;
        messagePtr->inScratch = false;
        messagePtr->opCode = opCode;
        enqueue(messagePtr);
        if (!corkPending) {
            corkPending = true;
            nodeData->corkState->pending.push_back(this);
        }
    }

    // Puts what is left of an op back at the head of the queue, in order.
    void requeueFront(SendOp *op) {
        if (!op->head) {
            return;
        }
        op->tail->nextMessage = messageQueue.head;
        messageQueue.head = op->head;
        if (!messageQueue.tail) {
            messageQueue.tail = op->tail;
        }
        messageQueue.totalLength += op->bytes;
    }

    template <class T>
    void closeSocket() {
        unregisterCork();
        uv_os_sock_t fd = getFd();
        Context *netContext = nodeData->netContext;
        stop(nodeData->loop);
        if (sendOp) {
            // The worker may be inside a send on this fd: closing it now could let the
            // number be reused by a new connection and receive our bytes. Orphan the op
            // and let its completion close the fd.
            sendOp->socket = nullptr;
            sendOp->closeFd = true;
            sendOp->needWake.store(true, std::memory_order_seq_cst);
            if (workerDeflateWindow) {
                sendOp->destroyWindow = workerDeflateWindow; // the worker may still be using it
                workerOwnsWindow = true;
            }
            sendOp = nullptr;
        } else {
            netContext->closeSocket(fd);
        }

        if (ssl) {
            SSL_free(ssl);
        }

        Poll::close(nodeData->loop, [](Poll *p) {
            delete (T *) p;
        });
    }

    bool isShuttingDown() {
        return state.shuttingDown;
    }

    friend class Node;
    friend struct NodeData;
};

struct ListenSocket : Socket {

    ListenSocket(NodeData *nodeData, Loop *loop, uv_os_sock_t fd, SSL *ssl) : Socket(nodeData, loop, fd, ssl) {

    }

    Timer *timer = nullptr;
    cS::TLS::Context sslContext;
};

}

#endif // SOCKET_CWS_H
