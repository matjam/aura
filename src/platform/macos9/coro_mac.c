/* Coroutines on Mac OS 9 (core/coro.h): the Thread Manager's cooperative
 * threads. Each coroutine is a thread with its own stack that the Thread
 * Manager switches to only when asked: a coroutine's thread is stopped
 * except while it is resumed, so neither WaitNextEvent nor the end of
 * another thread can schedule it. resume makes the thread ready and
 * yields to it; yield stops the thread and names the thread that resumed
 * it as the next to run: the main thread, or for a worker resumed from
 * the page's script coroutine (browser/page_doc.c, "Script that parks"),
 * that coroutine. A thread whose entry returns stops the same way and
 * waits for coro_destroy.
 *
 * The Multiversal Interfaces have no Threads.h, so the few calls are
 * declared here; they are in ThreadsLib, which every Mac OS 9 has. On
 * PowerPC the thread entry is a plain function pointer, not a UPP. */
#include "mac.h"
#include "core/coro.h"
#include "core/core.h"

typedef unsigned long MacThreadID;
typedef void *(*MacThreadEntry)(void *param);

enum { kMacCooperativeThread = 1 };
enum { kMacNewSuspend = 1, kMacCreateIfNeeded = 4 };
enum { kMacReadyThreadState = 0, kMacStoppedThreadState = 1 };
enum { kMacNoThreadID = 0, kMacCurrentThreadID = 1, kMacApplicationThreadID = 2 };

extern pascal OSErr NewThread(unsigned long style, MacThreadEntry entry, void *param, Size stack_size,
                              unsigned long options, void **result, MacThreadID *made);
extern pascal OSErr DisposeThread(MacThreadID thread, void *result, Boolean recycle);
extern pascal OSErr YieldToThread(MacThreadID thread);
extern pascal OSErr SetThreadState(MacThreadID thread, unsigned short state, MacThreadID suggested);
extern pascal OSErr GetCurrentThread(MacThreadID *thread);

typedef struct MacCoro {
    MacThreadID thread;
    MacThreadID resumer; /* the thread that resumed it: a worker's resumer is the page's script coroutine */
    void (*entry)(void *arg);
    void *arg;
    bool done;
} MacCoro;

static void *coro_thread(void *param)
{
    MacCoro *c = param;
    c->entry(c->arg);
    c->done = true;
    /* Back to the resumer, stopped until coro_destroy disposes of the
     * thread. Returning would let the Thread Manager run any ready
     * thread: with a worker resumed from the page's script coroutine,
     * both that coroutine and the main thread are ready. */
    for (;;)
        SetThreadState(kMacCurrentThreadID, kMacStoppedThreadState,
                       c->resumer != kMacNoThreadID ? c->resumer : kMacApplicationThreadID);
    return NULL;
}

static void *coro_create(void *ctx, size_t stack_size, void (*entry)(void *arg), void *arg)
{
    (void)ctx;
    MacCoro *c = au_calloc(sizeof *c);
    c->entry = entry;
    c->arg = arg;
    if (NewThread(kMacCooperativeThread, coro_thread, c, (Size)stack_size, kMacNewSuspend | kMacCreateIfNeeded, NULL,
                  &c->thread) != noErr) {
        au_free(c);
        return NULL;
    }
    return c;
}

static bool coro_resume(void *ctx, void *co)
{
    (void)ctx;
    MacCoro *c = co;
    if (c->done)
        return false;
    if (GetCurrentThread(&c->resumer) != noErr)
        c->resumer = kMacApplicationThreadID;
    if (SetThreadState(c->thread, kMacReadyThreadState, kMacNoThreadID) != noErr || YieldToThread(c->thread) != noErr)
        c->done = true; /* the thread is gone: nothing more can run on it */
    return !c->done;
}

static void coro_yield(void *ctx, void *co)
{
    (void)ctx;
    MacCoro *c = co;
    SetThreadState(kMacCurrentThreadID, kMacStoppedThreadState,
                   c->resumer != kMacNoThreadID ? c->resumer : kMacApplicationThreadID);
}

static void coro_destroy(void *ctx, void *co)
{
    (void)ctx;
    MacCoro *c = co;
    DisposeThread(c->thread, NULL, false); /* stopped: in coro_yield, or after its entry */
    au_free(c);
}

const AuCoroOps *mac_coro_ops(void)
{
    static const AuCoroOps ops = {NULL, coro_create, coro_resume, coro_yield, coro_destroy};
    return &ops;
}
