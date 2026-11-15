// Workers without coroutines (aura-host js --no-coro): a worker runs on
// the caller's stack, one task at a time. Messages, timers, fetch and
// close work; nothing can wait for the network (importScripts of a
// network or file URL, a synchronous XMLHttpRequest); a task that never
// returns is stopped by the long-script limit and the page goes on.

const next = (target, type = 'message') => new Promise((resolve) => target.addEventListener(type, resolve, { once: true }));
const blobWorker = (src) => new Worker(URL.createObjectURL(new Blob([src])));

promise_test('messages, timers and fetch', async () => {
    const data = new URL('resources/data.json', document.URL).href;
    const w = blobWorker(`onmessage = async (e) => {
        if (e.data === 'timer') setTimeout(() => postMessage('timed'), 5);
        else if (e.data === 'sync') { try { importScripts(${JSON.stringify(data)}); postMessage('imported'); } catch (err) { postMessage(err.name); } }
        else { const r = await fetch(e.data); postMessage((await r.json()).name); }
    };`);
    w.postMessage('timer');
    assert.equal((await next(w)).data, 'timed');
    w.postMessage(data);
    assert.equal((await next(w)).data, 'aura');
    w.postMessage('sync');
    assert.equal((await next(w)).data, 'NetworkError', 'no waiting for the network on the page stack');
    w.terminate();
});

promise_test('a task that never returns is stopped; the page goes on', async () => {
    const w = blobWorker('onmessage = (e) => { if (e.data === 1) for (;;) ; postMessage("alive"); };');
    w.postMessage(1);
    await delay(20);
    w.postMessage(2);
    assert.equal((await next(w)).data, 'alive', 'later tasks of the worker still run');
    w.terminate();
});
