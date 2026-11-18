/* Coroutines on the calling thread (core/coro.h, posix.h), with
 * makecontext and swapcontext: each coroutine has a stack of its own and
 * runs on the thread that resumes it, as the Thread Manager's cooperative
 * threads do on Mac OS 9. The GTK and Cocoa shells give these for page
 * script (AuBrowserHost.script_coro): the script calls the shell's
 * callbacks, and GTK and AppKit take calls only on the main thread, which
 * the thread coroutines of coro_posix.c are not.
 *
 * ucontext is deprecated on macOS but still there (arm64 and x86_64).
 * Under AddressSanitizer each switch is announced
 * (__sanitizer_start_switch_fiber), so that it follows the stacks. */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700 /* the ucontext routines need it on macOS */
#define _DARWIN_C_SOURCE  /* and MAP_ANON with it */
#endif
#include "posix.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UCORO_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(UCORO_ASAN)
#define UCORO_ASAN 1
#endif
#ifdef UCORO_ASAN
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **bottom_old, size_t *size_old);
#endif

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

typedef struct UCoro {
    ucontext_t ctx;    /* the coroutine's */
    ucontext_t caller; /* the resumer's, while the coroutine runs */
    void *map;         /* the stack and its guard page */
    size_t map_len;
    char *stack;
    size_t stack_len;
    bool done;
    void (*entry)(void *arg);
    void *arg;
#ifdef UCORO_ASAN
    void *fake;
    const void *caller_bottom;
    size_t caller_size;
#endif
} UCoro;

/* makecontext passes int arguments only: the coroutine starting is
 * handed over here, on the one thread that resumes it. */
static _Thread_local UCoro *starting;

static void trampoline(void)
{
    UCoro *c = starting;
#ifdef UCORO_ASAN
    __sanitizer_finish_switch_fiber(NULL, &c->caller_bottom, &c->caller_size);
#endif
    c->entry(c->arg);
    c->done = true;
#ifdef UCORO_ASAN
    __sanitizer_start_switch_fiber(NULL, c->caller_bottom, c->caller_size); /* NULL: this stack ends */
#endif
    /* returns to uc_link: the resumer */
}

static void *ucoro_create(void *ctx, size_t stack_size, void (*entry)(void *arg), void *arg)
{
    (void)ctx;
    UCoro *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (page < 4096)
        page = 4096;
    if (stack_size < (size_t)64 << 10)
        stack_size = (size_t)64 << 10;
    stack_size = (stack_size + page - 1) & ~(page - 1);
    /* a guard page below the stack: an overflow faults instead of
     * writing over the heap */
    c->map_len = stack_size + page;
    c->map = mmap(NULL, c->map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (c->map == MAP_FAILED) {
        free(c);
        return NULL;
    }
    mprotect(c->map, page, PROT_NONE);
    c->stack = (char *)c->map + page;
    c->stack_len = stack_size;
    c->entry = entry;
    c->arg = arg;
    if (getcontext(&c->ctx) != 0) {
        munmap(c->map, c->map_len);
        free(c);
        return NULL;
    }
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = c->stack_len;
    c->ctx.uc_link = &c->caller;
    makecontext(&c->ctx, trampoline, 0);
    return c;
}

static bool ucoro_resume(void *ctx, void *co)
{
    (void)ctx;
    UCoro *c = co;
    if (c->done)
        return false;
    starting = c;
#ifdef UCORO_ASAN
    void *fake = NULL;
    __sanitizer_start_switch_fiber(&fake, c->stack, c->stack_len);
#endif
    swapcontext(&c->caller, &c->ctx);
#ifdef UCORO_ASAN
    __sanitizer_finish_switch_fiber(fake, NULL, NULL);
#endif
    return !c->done;
}

static void ucoro_yield(void *ctx, void *co)
{
    (void)ctx;
    UCoro *c = co;
#ifdef UCORO_ASAN
    __sanitizer_start_switch_fiber(&c->fake, c->caller_bottom, c->caller_size);
#endif
    swapcontext(&c->ctx, &c->caller);
#ifdef UCORO_ASAN
    __sanitizer_finish_switch_fiber(c->fake, &c->caller_bottom, &c->caller_size);
#endif
}

static void ucoro_destroy(void *ctx, void *co)
{
    (void)ctx;
    UCoro *c = co;
    munmap(c->map, c->map_len);
    free(c);
}

const AuCoroOps *au_posix_ucoro_ops(void)
{
    static const AuCoroOps ops = {NULL, ucoro_create, ucoro_resume, ucoro_yield, ucoro_destroy};
    return &ops;
}
