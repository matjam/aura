// A worker for tests/js/workers.js: answers commands.
importScripts('worker_lib.js');
let count = 0;
onmessage = async (e) => {
    const m = e.data;
    count++;
    switch (m && m.cmd) {
    case 'echo': postMessage({ echo: m.value, count }); break;
    case 'types':
        postMessage({ date: m.value.date instanceof Date && m.value.date.getTime(), map: m.value.map instanceof Map && m.value.map.get('k'),
            set: m.value.set instanceof Set && m.value.set.has(3), re: m.value.re instanceof RegExp && m.value.re.source,
            bytes: m.value.bytes instanceof Uint8Array && Array.from(m.value.bytes), same: m.value.a === m.value.b,
            blob: m.value.blob instanceof Blob && m.value.blob.type, err: m.value.err instanceof RangeError && m.value.err.message,
            big: typeof m.value.big, undef: 'u' in m.value && m.value.u === undefined });
        break;
    case 'env':
        postMessage({ doc: typeof document, win: typeof window, self: self === globalThis, href: location.href, origin: location.origin,
            ua: navigator.userAgent, name, lib: libValue(), worker: typeof Worker, shared: typeof SharedWorker, sw: typeof ServiceWorker,
            scope: self instanceof DedicatedWorkerGlobalScope && self instanceof EventTarget, raf: typeof requestAnimationFrame,
            api: [typeof fetch, typeof XMLHttpRequest, typeof URL, typeof TextEncoder, typeof atob, typeof structuredClone,
                typeof queueMicrotask, typeof performance.now(), typeof crypto.getRandomValues, typeof setInterval, typeof console.log].join() });
        break;
    case 'transfer': {
        const b = m.buffer;
        const out = new Uint8Array(b).map((x) => x * 2).buffer;
        postMessage({ len: b.byteLength, out }, [out]);
        postMessage({ after: out.byteLength });
        break;
    }
    case 'port': e.ports[0].onmessage = (pe) => e.ports[0].postMessage('port:' + pe.data); break;
    case 'makeport': {
        const ch = new MessageChannel();
        ch.port1.onmessage = (pe) => ch.port1.postMessage({ back: pe.data });
        postMessage({ port: true }, [ch.port2]);
        break;
    }
    case 'fetch': {
        const r = await fetch(m.url);
        postMessage({ status: r.status, name: (await r.json()).name });
        break;
    }
    case 'xhr': {
        const x = new XMLHttpRequest();
        x.open('GET', m.url, false);
        x.send();
        postMessage({ status: x.status, text: x.responseText.length > 0 });
        break;
    }
    case 'digest': {
        const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(m.text));
        postMessage({ hex: Array.from(new Uint8Array(d), (b) => b.toString(16).padStart(2, '0')).join('') });
        break;
    }
    case 'timer': setTimeout(() => postMessage({ timer: true }), 5); break;
    case 'throw': setTimeout(() => { throw new Error('worker failure'); }, 0); break;
    case 'spin': { const t = Date.now(); let n = 0; while (Date.now() - t < m.ms) n++; postMessage({ spun: n > 0 }); break; }
    case 'forever': for (;;) count++;
    case 'close': postMessage({ closing: true }); close(); postMessage({ afterClose: true }); break;
    }
};
