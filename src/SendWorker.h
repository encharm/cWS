#ifndef CWS_SEND_WORKER_H
#define CWS_SEND_WORKER_H

// Send worker thread. The end-of-tick flush hands each socket's queued frames to
// one worker thread as an op; the worker does the gathered send syscall (the
// kernel's TCP work is charged to it, not to the JS thread) and posts the op back.
// Both directions are single-producer/single-consumer lock-free queues
// (deps/readerwriterqueue); the worker parks on a semaphore when idle and the main
// thread is woken through a uv_async. Disabled with CWS_SEND_THREAD=0.

#include <uv.h>

namespace cS {

struct SendWorker {
    static bool init(uv_loop_t *loop);
    static bool active();
    static const char *status();   // "active", or why not
    // Main thread only. Returns false if the queue is full; the caller then sends synchronously.
    static bool submit(void *op);
};

}

#endif // CWS_SEND_WORKER_H
