import { a } from './mod_a.mjs';
self.onmessage = (e) => postMessage({ module: true, a: typeof a, meta: import.meta.url.endsWith('worker_mod.mjs'), got: e.data });
