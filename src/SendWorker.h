#ifndef CWS_SEND_WORKER_H
#define CWS_SEND_WORKER_H

// Send worker thread. The end-of-tick flush hands each socket's queued frames to
// one worker thread as an op; the worker does the gathered send syscall (the
// kernel's TCP work is charged to it, not to the JS thread) and posts the op back.
// Both directions are single-producer/single-consumer lock-free queues
// (deps/readerwriterqueue). Hand-offs are batched per loop iteration: submit() only
// enqueues, flush() (called from the loop hooks and after completions) wakes the
// worker once for everything queued, the worker drains all of it and wakes the main
// thread once through a uv_async. Ops come from a freelist, so a socket's 8 KB op is
// allocated once, not per tick. Disabled with CWS_SEND_THREAD=0.

#include <uv.h>

namespace cS {

struct SendWorker {
    static bool init(uv_loop_t *loop);
    static bool active();
    static const char *status();   // "active", or why not
    // Main thread only. Returns false if the queue is full; the caller then sends synchronously.
    static bool submit(void *op);
    // Main thread only: wakes the worker if submit() queued anything since the last flush.
    static void flush();
    // Loop hooks. beforePoll (end of the prepare hook) marks the loop as about to block and
    // drains completions one last time; afterPoll (start of the check hook) clears the mark
    // and drains. The worker wakes the loop through the uv_async only while it is marked
    // idle, so a busy loop handles completions in its hooks without any syscall. The worker
    // spins for CWS_SEND_THREAD_SPIN_US (default 50) before sleeping, so back-to-back ticks
    // never pay a futex wake on the JS thread.
    static void beforePoll();
    static void afterPoll();
    // Main thread only: op freelist (Socket::SendOp, typed as void* to keep this header light).
    static void *allocOp();
    static void freeOp(void *op);
};

}

#endif // CWS_SEND_WORKER_H
