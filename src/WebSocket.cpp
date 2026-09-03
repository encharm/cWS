#include "WebSocket.h"
#include "Group.h"
#include "Hub.h"

namespace cWS {

template <bool isServer>
WebSocket<isServer>::WebSocket(bool perMessageDeflate, cS::Socket *socket) : cS::Socket(std::move(*socket)) {
    compressionStatus = perMessageDeflate ? CompressionStatus::ENABLED : CompressionStatus::DISABLED;
    setCorkable(true);

    // if we are created in a group with sliding deflate window allocate it here
    if (Group<isServer>::from(this)->extensionOptions & SLIDING_DEFLATE_WINDOW) {
        Group<isServer> *group = Group<isServer>::from(this);
        slidingDeflateWindow = Hub::allocateDefaultCompressor(group->deflateLevel, group->deflateWindowBits, group->deflateMemLevel);
    }
    workerDeflateWindow = slidingDeflateWindow;
    materializeCb = WebSocket<isServer>::materialize;
}

/*
 * Frames and sends a WebSocket message.
 *
 * Hints: Consider using any of the prepare function if any of their
 * use cases match what you are trying to achieve (pub/sub, broadcast)
 *
 * Thread safe
 *
 */
template <bool isServer>
void WebSocket<isServer>::send(const char *message, size_t length, OpCode opCode, void(*callback)(WebSocket<isServer> *webSocket, void *data, bool cancelled, void *reserved), void *callbackData, bool compress) {

#ifdef CWS_THREADSAFE
    std::lock_guard<std::recursive_mutex> lockGuard(*nodeData->asyncMutex);
    if (isClosed()) {
        if (callback) {
            callback(this, callbackData, true, nullptr);
        }
        return;
    }
#endif

    const int HEADER_LENGTH = WebSocketProtocol<!isServer, WebSocket<!isServer>>::LONG_MESSAGE_HEADER;

    struct TransformData {
        OpCode opCode;
        bool compress;
        WebSocket<isServer> *s;
    } transformData = {opCode, compress && compressionStatus == WebSocket<isServer>::CompressionStatus::ENABLED && opCode < 3, this};

    struct WebSocketTransformer {
        static size_t estimate(const char *data, size_t length) {
            return length + HEADER_LENGTH;
        }

        static size_t transform(const char *src, char *dst, size_t length, TransformData transformData) {
            if (transformData.compress) {
                char *deflated = Group<isServer>::from(transformData.s)->hub->deflate((char *) src, length, (zlib::Stream *) transformData.s->slidingDeflateWindow);
                return WebSocketProtocol<isServer, WebSocket<isServer>>::formatMessage(dst, deflated, length, transformData.opCode, length, true);
            }

            return WebSocketProtocol<isServer, WebSocket<isServer>>::formatMessage(dst, src, length, transformData.opCode, length, false);
        }
    };

    if (transformData.compress && !ssl && corkActive() && cS::SendWorker::active()) {
        // deflate + framing happen on the send worker, not on the JS thread
        enqueueCompressPending(message, length, (unsigned char) opCode, (void(*)(void *, void *, bool, void *)) callback, callbackData);
        return;
    }

    sendTransformed<WebSocketTransformer>((char *) message, length, (void(*)(void *, void *, bool, void *)) callback, callbackData, transformData);
}

// Main-thread fallback for a compressPending message that a synchronous write path reached
// (same-tick terminate, full worker queue, drain loop after a short write).
template <bool isServer>
void WebSocket<isServer>::materialize(cS::Socket *s, cS::Socket::Queue::Message *m) {
    WebSocket<isServer> *webSocket = static_cast<WebSocket<isServer> *>(s);
    Hub *hub = Group<isServer>::from(webSocket)->hub;
    zlib::Stream *stream = webSocket->slidingDeflateWindow ? (zlib::Stream *) webSocket->slidingDeflateWindow : hub->deflationStream;
    cS::Socket::materializeOnMain(s, m, stream, !webSocket->slidingDeflateWindow, hub->zlibBuffer, Hub::LARGE_BUFFER_SIZE, hub->dynamicZlibBuffer);
}

template <bool isServer>
SharedPayload *WebSocket<isServer>::prepareShared(const char *data, size_t length) {
    SharedPayload *payload = new SharedPayload;
    payload->raw.assign(data, length);
    return payload;
}

namespace {
struct SharedCallback {
    void (*callback)(void *socket, void *data, bool cancelled, void *reserved);
    void *data;
};

// Completion of the shared part of a sendShared() frame: drops the payload reference,
// then runs the caller's callback. `socket` is null when the send was orphaned by a close.
void sharedSent(void *socket, void *data, bool cancelled, void *reserved) {
    SharedPayload::unref((SharedPayload *) data);
    if (reserved) {
        SharedCallback *c = (SharedCallback *) reserved;
        c->callback(socket, c->data, cancelled, nullptr);
        delete c;
    }
}
}

/*
 * Sends prefix + payload as one frame. The frame is queued as two messages: an owned
 * one with the header and the prefix, and a borrowed one pointing into the payload, so
 * nothing is copied or compressed per recipient.
 *
 * Compressed: the prefix goes out as DEFLATE stored blocks in front of the payload's
 * pre-built blocks. That is valid because those blocks were produced from the payload
 * alone, so no match reaches back before their start. It needs a socket without context
 * takeover (no sliding window): with takeover the per-socket window would not contain
 * these bytes, so that case, like the client role (masking rewrites every byte), takes
 * the regular copying path.
 */
template <bool isServer>
void WebSocket<isServer>::sendShared(const char *prefix, size_t prefixLength, SharedPayload *payload, OpCode opCode, bool compress,
                                     void(*callback)(WebSocket<isServer> *webSocket, void *data, bool cancelled, void *reserved), void *callbackData) {
    bool deflate = compress && compressionStatus == WebSocket<isServer>::CompressionStatus::ENABLED && opCode < 3;
    if (!isServer || (deflate && slidingDeflateWindow) || payload->raw.empty()) {
        std::string whole;
        whole.reserve(prefixLength + payload->raw.size());
        whole.append(prefix, prefixLength).append(payload->raw);
        send(whole.data(), whole.size(), opCode, callback, callbackData, compress);
        return;
    }
    if (isClosed()) {
        if (callback) {
            callback(this, callbackData, true, nullptr);
        }
        return;
    }

    const char *body;
    size_t bodyLength;
    if (deflate) {
        if (!payload->deflatedReady) {
            size_t length = payload->raw.size();
            char *out = Group<isServer>::from(this)->hub->deflate((char *) payload->raw.data(), length, nullptr);
            payload->deflated.assign(out, length);
            payload->deflatedReady = true;
        }
        body = payload->deflated.data();
        bodyLength = payload->deflated.size();
    } else {
        body = payload->raw.data();
        bodyLength = payload->raw.size();
    }

    // a stored block carries at most 65535 bytes: 1 byte header, LEN, NLEN
    const size_t STORED_MAX = 65535;
    size_t storedBlocks = deflate ? (prefixLength + STORED_MAX - 1) / STORED_MAX : 0;
    size_t prefixPart = prefixLength + storedBlocks * 5;
    const size_t MAX_HEADER = 14;

    // Header + prefix: into the tail slab when corked (no allocation, one iovec with the
    // frames before it), else its own heap message.
    bool corked = corkActive();
    Queue::Message *first = corked ? slabWithSpace(MAX_HEADER + prefixPart) : allocMessage(MAX_HEADER + prefixPart);
    char *dst = (char *) first->data + (corked ? first->length : 0);
    char *p = dst + WebSocketProtocol<isServer, WebSocket<isServer>>::formatMessage(dst, dst, 0, opCode, prefixPart + bodyLength, deflate);
    if (deflate) {
        const char *src = prefix;
        for (size_t remaining = prefixLength; remaining; ) {
            size_t n = remaining > STORED_MAX ? STORED_MAX : remaining;
            *p++ = 0; // BFINAL=0, BTYPE=00, already byte aligned
            *p++ = (char) (n & 0xff);
            *p++ = (char) (n >> 8);
            *p++ = (char) (~n & 0xff);
            *p++ = (char) ((~n >> 8) & 0xff);
            memcpy(p, src, n);
            p += n;
            src += n;
            remaining -= n;
        }
    } else {
        memcpy(p, prefix, prefixLength);
        p += prefixLength;
    }
    if (corked) {
        first->length += (size_t) (p - dst);
        messageQueue.totalLength += (size_t) (p - dst);
    } else {
        first->length = (size_t) (p - dst);
    }

    int memoryIndex = nodeData->getMemoryBlockIndex(sizeof(Queue::Message));
    Queue::Message *second = (Queue::Message *) nodeData->getSmallMemoryBlock(memoryIndex);
    second->data = body;
    second->length = bodyLength;
    second->nextMessage = nullptr;
    second->poolIndex = memoryIndex;
    second->ownsData = false;
    second->compressPending = false;
    second->run = false;
    second->inScratch = false;
    second->rawOwned = nullptr;
    second->opCode = 0;
    second->callback = sharedSent;
    second->callbackData = payload;
    second->reserved = callback ? new SharedCallback{(void(*)(void *, void *, bool, void *)) callback, callbackData} : nullptr;
    payload->references.fetch_add(1, std::memory_order_relaxed);

    if (corked) {
        enqueue(second); // `first` is (part of) the slab already in the queue
        if (!corkPending) {
            corkPending = true;
            nodeData->corkState->pending.push_back(this);
        }
        return;
    }

    // no write corking (CWS_CORK=0, TLS): write now, queue what does not fit, keep order
    bool wasTransferred;
    if (!write(first, wasTransferred)) {
        freeMessage(first);
        void *reserved = second->reserved;
        nodeData->freeSmallMemoryBlock((char *) second, memoryIndex);
        sharedSent(this, payload, true, reserved);
        return;
    }
    if (!wasTransferred) {
        freeMessage(first);
    }
    if (!write(second, wasTransferred)) {
        void *reserved = second->reserved;
        nodeData->freeSmallMemoryBlock((char *) second, memoryIndex);
        sharedSent(this, payload, true, reserved);
        return;
    }
    if (!wasTransferred) {
        void *reserved = second->reserved;
        nodeData->freeSmallMemoryBlock((char *) second, memoryIndex);
        sharedSent(this, payload, false, reserved);
    }
}

/*
 * Prepares a single message for use with sendPrepared.
 *
 * Hints: Useful in cases where you need to send the same message to many
 * recipients. Do not use when only sending one message.
 *
 * Thread safe
 *
 */
template <bool isServer>
typename WebSocket<isServer>::PreparedMessage *WebSocket<isServer>::prepareMessage(char *data, size_t length, OpCode opCode, bool compressed, void(*callback)(WebSocket<isServer> *webSocket, void *data, bool cancelled, void *reserved)) {
    PreparedMessage *preparedMessage = new PreparedMessage;
    preparedMessage->buffer = new char[length + 10];
    preparedMessage->length = WebSocketProtocol<isServer, WebSocket<isServer>>::formatMessage(preparedMessage->buffer, data, length, opCode, length, compressed);
    preparedMessage->references = 1;
    preparedMessage->callback = (void(*)(void *, void *, bool, void *)) callback;
    return preparedMessage;
}

/*
 * Prepares a batch of messages to send as one single TCP packet / syscall.
 *
 * Hints: Useful when doing pub/sub-like broadcasts where many recipients should receive many
 * messages. Do not use if only sending one message.
 *
 * Thread safe
 *
 */
template <bool isServer>
typename WebSocket<isServer>::PreparedMessage *WebSocket<isServer>::prepareMessageBatch(std::vector<std::string> &messages, std::vector<int> &excludedMessages, OpCode opCode, bool compressed, void (*callback)(WebSocket<isServer> *, void *, bool, void *))
{
    // should be sent in!
    size_t batchLength = 0;
    for (size_t i = 0; i < messages.size(); i++) {
        batchLength += messages[i].length();
    }

    PreparedMessage *preparedMessage = new PreparedMessage;
    preparedMessage->buffer = new char[batchLength + 10 * messages.size()];

    int offset = 0;
    for (size_t i = 0; i < messages.size(); i++) {
        offset += WebSocketProtocol<isServer, WebSocket<isServer>>::formatMessage(preparedMessage->buffer + offset, messages[i].data(), messages[i].length(), opCode, messages[i].length(), compressed);
    }
    preparedMessage->length = offset;
    preparedMessage->references = 1;
    preparedMessage->callback = (void(*)(void *, void *, bool, void *)) callback;
    return preparedMessage;
}

/*
 * Sends a prepared message.
 *
 * Hints: Used to improve broadcasting and similar use cases where the same
 * message is sent to multiple recipients. Do not used if only sending one message
 * in total.
 *
 * Warning: Modifies passed PreparedMessage and is thus not thread safe. Other
 * data is also modified and it makes sense to not make this function thread-safe
 * since it is a central part in broadcasting and other high-perf code paths.
 *
 */
template <bool isServer>
void WebSocket<isServer>::sendPrepared(typename WebSocket<isServer>::PreparedMessage *preparedMessage, void *callbackData) {
    // todo: see if this can be made a transformer instead
    preparedMessage->references++;
    void (*callback)(void *webSocket, void *userData, bool cancelled, void *reserved) = [](void *webSocket, void *userData, bool cancelled, void *reserved) {
        PreparedMessage *preparedMessage = (PreparedMessage *) userData;
        bool lastReference = !--preparedMessage->references;

        if (preparedMessage->callback) {
            preparedMessage->callback(webSocket, reserved, cancelled, (void *) lastReference);
        }

        if (lastReference) {
            delete [] preparedMessage->buffer;
            delete preparedMessage;
        }
    };

    // candidate for fixed size pool allocator
    int memoryLength = sizeof(Queue::Message);
    int memoryIndex = nodeData->getMemoryBlockIndex(memoryLength);

    Queue::Message *messagePtr = (Queue::Message *) nodeData->getSmallMemoryBlock(memoryIndex);
    messagePtr->data = preparedMessage->buffer;
    messagePtr->length = preparedMessage->length;
    messagePtr->nextMessage = nullptr;
    messagePtr->callback = nullptr;
    messagePtr->callbackData = nullptr;
    messagePtr->reserved = nullptr;
    messagePtr->poolIndex = memoryIndex;
    messagePtr->ownsData = false;      // pool blocks are reused: every flag must be set
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
                callback(this, preparedMessage, false, callbackData);
            }
        } else {
            messagePtr->callback = callback;
            messagePtr->callbackData = preparedMessage;
            messagePtr->reserved = callbackData;
        }
    } else {
        nodeData->freeSmallMemoryBlock((char *) messagePtr, memoryIndex);
        if (callback) {
            callback(this, preparedMessage, true, callbackData);
        }
    }
}

/*
 * Decrements the reference count of passed PreparedMessage. On zero references
 * the memory will be deleted.
 *
 * Hints: Used together with prepareMessage, prepareMessageBatch and similar calls.
 *
 * Warning: Will modify passed PrepareMessage and is thus not thread safe by itself.
 *
 */
template <bool isServer>
void WebSocket<isServer>::finalizeMessage(typename WebSocket<isServer>::PreparedMessage *preparedMessage) {
    if (!--preparedMessage->references) {
        delete [] preparedMessage->buffer;
        delete preparedMessage;
    }
}

template <bool isServer>
cS::Socket *WebSocket<isServer>::onData(cS::Socket *s, char *data, size_t length) {
    WebSocket<isServer> *webSocket = static_cast<WebSocket<isServer> *>(s);

    webSocket->hasOutstandingPong = false;
    if (!webSocket->isShuttingDown()) {
        // The kernel-level cork (TCP_CORK/TCP_NOPUSH) around a read only helps when
        // replies are written immediately; with write corking they leave in one gathered
        // write at the end of the tick, so skip the two setsockopt calls per read.
        bool kernelCork = !webSocket->corkActive();
        if (kernelCork) {
            webSocket->cork(true);
        }
        WebSocketProtocol<isServer, WebSocket<isServer>>::consume(data, (unsigned int) length, webSocket);
        if (kernelCork && !webSocket->isClosed()) {
            webSocket->cork(false);
        }
    }

    return webSocket;
}

/*
 * Immediately terminates this WebSocket. Will call onDisconnection of its Group.
 *
 * Hints: Close code will be 1006 and message will be empty.
 *
 */
template <bool isServer>
void WebSocket<isServer>::terminate() {

#ifdef CWS_THREADSAFE
    std::lock_guard<std::recursive_mutex> lockGuard(*nodeData->asyncMutex);
#endif
    if (isClosed()) {
        return;
    }

    WebSocket<isServer>::onEnd(this);
}

/*
 * Transfers this WebSocket from its current Group to specified Group.
 *
 * Receiving Group has to have called listen(cWS::TRANSFERS) prior.
 *
 * Hints: Useful to implement subprotocols on the same thread and Loop
 * or to transfer WebSockets between threads at any point (dynamic load balancing).
 *
 * Warning: From the point of call to the point of onTransfer, this WebSocket
 * is invalid and cannot be used. What you put in is not guaranteed to be what you
 * get in onTransfer, the only guaranteed consistency is passed userData is the userData
 * of given WebSocket in onTransfer. Use setUserData and getUserData to identify the WebSocket.
 */
template <bool isServer>
void WebSocket<isServer>::transfer(Group<isServer> *group) {
    Group<isServer>::from(this)->removeWebSocket(this);
    if (group->loop == Group<isServer>::from(this)->loop) {
        // fast path
        this->nodeData = group;
        Group<isServer>::from(this)->addWebSocket(this);
        Group<isServer>::from(this)->transferHandler(this);
    } else {
        // slow path
        cS::Socket::transfer((cS::NodeData *) group, [](Poll *p) {
            WebSocket<isServer> *webSocket = (WebSocket<isServer> *) p;
            Group<isServer>::from(webSocket)->addWebSocket(webSocket);
            Group<isServer>::from(webSocket)->transferHandler(webSocket);
        });
    }
}

/*
 * Immediately calls onDisconnection of its Group and begins a passive
 * WebSocket closedown handshake in the background (might succeed or not,
 * we don't care).
 *
 * Hints: Close code and message will be what you pass yourself.
 *
 */
template <bool isServer>
void WebSocket<isServer>::close(int code, const char *message, size_t length) {
    // startTimeout is not thread safe

    static const int MAX_CLOSE_PAYLOAD = 123;
    length = std::min<size_t>(MAX_CLOSE_PAYLOAD, length);
    Group<isServer>::from(this)->removeWebSocket(this);
    Group<isServer>::from(this)->disconnectionHandler(this, code, (char *) message, length);
    setShuttingDown(true);

    // todo: using the shared timer in the group, we can skip creating a new timer per socket
    // only this line and the one in Hub::connect uses the timeout feature
    startTimeout<WebSocket<isServer>::onEnd>();

    char closePayload[MAX_CLOSE_PAYLOAD + 2];
    int closePayloadLength = (int) WebSocketProtocol<isServer, WebSocket<isServer>>::formatClosePayload(closePayload, code, message, length);
    send(closePayload, closePayloadLength, OpCode::CLOSE, [](WebSocket<isServer> *p, void *data, bool cancelled, void *reserved) {
        if (!cancelled) {
            p->shutdown();
        }
    });
}

template <bool isServer>
void WebSocket<isServer>::onEnd(cS::Socket *s) {
    WebSocket<isServer> *webSocket = static_cast<WebSocket<isServer> *>(s);

    webSocket->flushCorkedOnClose();

    if (!webSocket->isShuttingDown()) {
        Group<isServer>::from(webSocket)->removeWebSocket(webSocket);
        Group<isServer>::from(webSocket)->disconnectionHandler(webSocket, 1006, nullptr, 0);
    } else {
        webSocket->cancelTimeout();
    }

    webSocket->template closeSocket<WebSocket<isServer>>();

    while (!webSocket->messageQueue.empty()) {
        Queue::Message *message = webSocket->messageQueue.front();
        if (message->callback) {
            message->callback(nullptr, message->callbackData, true, nullptr);
        }
        webSocket->messageQueue.pop(webSocket->nodeData);
    }

    webSocket->nodeData->clearPendingPollChanges(webSocket);

    // remove any per-websocket zlib memory (unless an orphaned send op took it over)
    if (webSocket->slidingDeflateWindow && !webSocket->workerOwnsWindow) {
        // this relates to Hub::allocateDefaultCompressor
        zlib::destroy((zlib::Stream *) webSocket->slidingDeflateWindow);
        webSocket->slidingDeflateWindow = nullptr;
    }
}

template <bool isServer>
bool WebSocket<isServer>::handleFragment(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, WebSocketState<isServer> *webSocketState) {
    WebSocket<isServer> *webSocket = static_cast<WebSocket<isServer> *>(webSocketState);
    Group<isServer> *group = Group<isServer>::from(webSocket);

    if (opCode < 3) {
        if (!remainingBytes && fin && !webSocket->fragmentBuffer.length()) {
            if (webSocket->compressionStatus == WebSocket<isServer>::CompressionStatus::COMPRESSED_FRAME) {
                    webSocket->compressionStatus = WebSocket<isServer>::CompressionStatus::ENABLED;
                    data = group->hub->inflate(data, length, group->maxPayload);
                    if (!data) {
                        forceClose(webSocketState);
                        return true;
                    }
            }

            if (opCode == 1 && !WebSocketProtocol<isServer, WebSocket<isServer>>::isValidUtf8((unsigned char *) data, length)) {
                forceClose(webSocketState);
                return true;
            }

            group->messageHandler(webSocket, data, length, (OpCode) opCode);
            if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                return true;
            }
        } else {
            webSocket->fragmentBuffer.append(data, length);
            if (!remainingBytes && fin) {
                length = webSocket->fragmentBuffer.length();
                if (webSocket->compressionStatus == WebSocket<isServer>::CompressionStatus::COMPRESSED_FRAME) {
                        webSocket->compressionStatus = WebSocket<isServer>::CompressionStatus::ENABLED;
                        webSocket->fragmentBuffer.append("....");
                        data = group->hub->inflate((char *) webSocket->fragmentBuffer.data(), length, group->maxPayload);
                        if (!data) {
                            forceClose(webSocketState);
                            return true;
                        }
                } else {
                    data = (char *) webSocket->fragmentBuffer.data();
                }

                if (opCode == 1 && !WebSocketProtocol<isServer, WebSocket<isServer>>::isValidUtf8((unsigned char *) data, length)) {
                    forceClose(webSocketState);
                    return true;
                }

                group->messageHandler(webSocket, data, length, (OpCode) opCode);
                if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                    return true;
                }
                webSocket->fragmentBuffer.clear();
            }
        }
    } else {
        if (!remainingBytes && fin && !webSocket->controlTipLength) {
            if (opCode == CLOSE) {
                typename WebSocketProtocol<isServer, WebSocket<isServer>>::CloseFrame closeFrame = WebSocketProtocol<isServer, WebSocket<isServer>>::parseClosePayload(data, length);
                webSocket->close(closeFrame.code, closeFrame.message, closeFrame.length);
                return true;
            } else {
                if (opCode == PING) {
                    webSocket->send(data, length, (OpCode) OpCode::PONG);
                    group->pingHandler(webSocket, data, length);
                    if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                        return true;
                    }
                } else if (opCode == PONG) {
                    group->pongHandler(webSocket, data, length);
                    if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                        return true;
                    }
                }
            }
        } else {
            webSocket->fragmentBuffer.append(data, length);
            webSocket->controlTipLength += length;

            if (!remainingBytes && fin) {
                char *controlBuffer = (char *) webSocket->fragmentBuffer.data() + webSocket->fragmentBuffer.length() - webSocket->controlTipLength;
                if (opCode == CLOSE) {
                    typename WebSocketProtocol<isServer, WebSocket<isServer>>::CloseFrame closeFrame = WebSocketProtocol<isServer, WebSocket<isServer>>::parseClosePayload(controlBuffer, webSocket->controlTipLength);
                    webSocket->close(closeFrame.code, closeFrame.message, closeFrame.length);
                    return true;
                } else {
                    if (opCode == PING) {
                        webSocket->send(controlBuffer, webSocket->controlTipLength, (OpCode) OpCode::PONG);
                        group->pingHandler(webSocket, controlBuffer, webSocket->controlTipLength);
                        if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                            return true;
                        }
                    } else if (opCode == PONG) {
                        group->pongHandler(webSocket, controlBuffer, webSocket->controlTipLength);
                        if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                            return true;
                        }
                    }
                }

                webSocket->fragmentBuffer.resize(webSocket->fragmentBuffer.length() - webSocket->controlTipLength);
                webSocket->controlTipLength = 0;
            }
        }
    }

    return false;
}

template struct WebSocket<SERVER>;
template struct WebSocket<CLIENT>;

}
