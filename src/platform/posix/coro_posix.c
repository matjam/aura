/* Coroutines over POSIX threads (core/coro.h, posix.h): each coroutine is
 * a thread that runs only while the thread that resumed it waits, so
 * there is one thread of control at any time, as with the Thread Manager
 * on Mac OS 9. Workers and the host tool use these; page script in the
 * GTK and Cocoa shells, which must call the toolkit on the main thread,
 * uses the ucontext stacks of coro_ucontext.c. */
#include "posix.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct PosixCoro {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int turn; /* 0: the resumer runs; 1: the coroutine runs */
    bool done;
    void (*entry)(void *arg);
    void *arg;
} PosixCoro;

static void *coro_thread(void *p)
{
    PosixCoro *c = p;
    pthread_mutex_lock(&c->mu);
    while (c->turn != 1)
        pthread_cond_wait(&c->cv, &c->mu);
    pthread_mutex_unlock(&c->mu);
    c->entry(c->arg);
    pthread_mutex_lock(&c->mu);
    c->done = true;
    c->turn = 0;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

static void *coro_create(void *ctx, size_t stack_size, void (*entry)(void *arg), void *arg)
{
    (void)ctx;
    PosixCoro *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->entry = entry;
    c->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size < (size_t)64 << 10)
        stack_size = (size_t)64 << 10;
    /* whole pages, and room for the C library's own frames */
    stack_size = (stack_size + 0x3fff) & ~(size_t)0x3fff;
    pthread_attr_setstacksize(&attr, stack_size);
    int err = pthread_create(&c->thread, &attr, coro_thread, c);
    pthread_attr_destroy(&attr);
    if (err) {
        pthread_mutex_destroy(&c->mu);
        pthread_cond_destroy(&c->cv);
        free(c);
        return NULL;
    }
    return c;
}

static bool coro_resume(void *ctx, void *co)
{
    (void)ctx;
    PosixCoro *c = co;
    pthread_mutex_lock(&c->mu);
    if (!c->done) {
        c->turn = 1;
        pthread_cond_broadcast(&c->cv);
        while (c->turn != 0)
            pthread_cond_wait(&c->cv, &c->mu);
    }
    bool alive = !c->done;
    pthread_mutex_unlock(&c->mu);
    return alive;
}

static void coro_yield(void *ctx, void *co)
{
    (void)ctx;
    PosixCoro *c = co;
    pthread_mutex_lock(&c->mu);
    c->turn = 0;
    pthread_cond_broadcast(&c->cv);
    while (c->turn != 1)
        pthread_cond_wait(&c->cv, &c->mu);
    pthread_mutex_unlock(&c->mu);
}

static void coro_destroy(void *ctx, void *co)
{
    (void)ctx;
    PosixCoro *c = co;
    pthread_join(c->thread, NULL);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c);
}

const AuCoroOps *au_posix_coro_ops(void)
{
    static const AuCoroOps ops = {NULL, coro_create, coro_resume, coro_yield, coro_destroy};
    return &ops;
}
