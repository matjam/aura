# Workers

This document covers dedicated workers: their runtimes, how messages
cross, scheduling with coroutines and the limits. Read it before
changing `src/js/worker.c`, the worker prelude or a platform's
coroutines.

`new Worker(url)` starts a dedicated worker. To change workers, work in
`src/js/worker.c` (the runtime, scheduling, messages),
`src/js/prelude_worker.js` (the worker global) and
`src/js/prelude_workers.js` (the `Worker` object in the page).
`SharedWorker` and `ServiceWorker` are not defined, and a worker cannot
start workers.

## To try it on the host

Run `aura-host js FILE.js` (`--no-coro` runs workers without their own
stacks) or `aura-host page --real-time PAGE.html`: the page tool's
default clock moves only between rounds, and a worker's slices need one
that moves while script runs.

## A runtime of its own

Each worker is a QuickJS runtime with the worker prelude
([`javascript.md`](javascript.md), "The prelude") and no document, with
its own memory limit (`AuJsSettings.worker_memory_limit`, 24 MB when
the setting is 0).

- The page fetches the script (same origin, or a `blob:` or `data:`
  URL) and hands the text over.
- Messages cross as bytes: `scEncode` in `prelude_web.js` turns a value
  into a plain tree, `JS_WriteObject` writes it, and the other side
  reads it and builds the value.
- An `ArrayBuffer` in the transfer list is copied and then emptied in
  the sender.
- A `MessagePort` crosses as a channel number (`JsPortChan` in
  `worker.c`), in either direction.

## Scheduling

Workers run only inside `au_js_run_workers`, which `au_docscript_run`
calls after the document's own tasks, 12 ms per call for all of a
document's workers in turn.

**With coroutines** (`AuCoroOps`, `src/core/coro.h`) each worker has
its own stack: when its 8 ms slice is used up, the script engine's
interrupt check yields to the event loop from wherever the script is,
and the next slice goes on from there. `importScripts` and a
synchronous `XMLHttpRequest` park the stack the same way until the
network answers. `terminate()` and the end of the document set a flag
and resume the worker, whose interrupt check then unwinds the script.

**Without coroutines** a worker runs one task per slice on the caller's
stack: a long task blocks the page until the long-script limit stops
it, and nothing can wait for the network.

## Coroutines come from the platform

Coroutines come from the platform, as sockets do:
`AuBrowserHost.coro`, then `AuPageEnv.coro`, `AuDocViewCallbacks.coro`
and `AuJsHooks.coro`.

- Mac OS 9 uses the Thread Manager's cooperative threads (`coro_mac.c`:
  a worker's thread is stopped except while it is resumed).
- The host tools use a thread that runs only while its resumer waits
  (`coro_host.c`).
- Only one stack runs at a time, so engine code needs no locks.
- Everything that touches a worker's runtime runs on the worker's
  stack; the page's side only appends C data to the worker's queue
  (what the embedder reports for a worker's requests is kept and
  replayed there, `js_worker_defer_fetch`).

## Limits

8 workers per page (`max_workers`; a ninth fires `error`), a 256 KB
stack each (`worker_stack`). Workers end with their document, wait
while the document is hidden (a kept document is not called), and count
in `au_js_stats` and so in the page's memory estimate.

## Tests

- `tests/js/workers.js`: messages, clones, transfers, ports, the worker
  global, `importScripts`, `blob:`, `data:` and module workers, errors,
  `close`, `terminate` of an endless loop, the limit.
- `tests/js/workers_inline.js`: without coroutines.
- `tests/js/pages/worker_pow.html`: a proof of work while page timers
  fire.

Run them under the sanitizers after a change to lifetimes.

## Measured

| Where | What | Figure |
|---|---|---|
| Host (RelWithDebInfo) | a worker's script heap | 587 KB, plus its 256 KB stack |
| Host | the worker prelude's bytecode | 98 KB (the page's is 258 KB, a document's context 1.4 MB) |
| Host | a SHA-256 search of 34,765 hashes in a worker | 778 ms, while a 5 ms page timer fired 47 times |
| Guest (Enforcer base) | 14,083 hashes | 13.3 s, while a 250 ms page timer fired 49 times of a possible 53 |

## Not done

See [`known-gaps.md`](known-gaps.md#workers).
