
/* ---------------- Worker (the page side) ----------------
 * new Worker(url) starts a dedicated worker: another script runtime with
 * no document (src/js/worker.c, prelude_worker.js). The script is fetched
 * here, by the document, and handed over; a blob: URL of this document
 * and a data: URL work too. SharedWorker and ServiceWorker are not
 * defined (pages feature-detect them). */
const workersById = new Map();
class Worker extends EventTarget {
    constructor(url, opts) {
        if (!arguments.length) throw new TypeError('Worker: 1 argument required');
        super();
        let href;
        try { href = new URL(str(url), N.docUrl(1)).href; } catch (e) { throw domErr('Worker: invalid URL', 'SyntaxError'); }
        const type = opts && opts.type !== undefined ? str(opts.type) : 'classic';
        if (type !== 'classic' && type !== 'module') throw new TypeError('Worker: invalid type ' + type);
        const name = opts && opts.name !== undefined ? str(opts.name) : '';
        const cred = opts && opts.credentials !== undefined ? str(opts.credentials) : 'same-origin';
        if (!(cred in credNum)) throw new TypeError('Worker: invalid credentials');
        const local = /^(blob|data):/i.test(href);
        if (!local && !N.sameOrigin(href)) throw domErr('Worker: the script at ' + href + ' is of another origin', 'SecurityError');
        this._id = N.workerCreate(href, name, type === 'module', /^data:/i.test(href));
        const fail = (msg) => {
            if (this._id) { N.workerSource(this._id, null); workersById.delete(this._id); this._id = 0; }
            N.print(3, 'Worker: ' + msg);
            fire(this, new Event('error', { cancelable: true }));
        };
        if (!this._id) { N.queueTask(() => fail('too many workers in this page')); return; }
        workersById.set(this._id, this);
        const chunks = [];
        let status = 0, final = href;
        try {
            /* mode same-origin (2), a worker's script (4) */
            startFetch('GET', href.replace(/#.*$/, ''), [], null, credNum[cred], 2, 4, {
                head(s, t, h, u) { status = s; if (u) final = u; },
                data(b) { chunks.push(b); },
                done: (ok, msg) => {
                    if (!this._id) return;
                    if (!ok || status < 200 || status > 299) { fail('could not load ' + href + (msg ? ' (' + msg + ')' : '')); return; }
                    N.workerSource(this._id, N.utf8Decode(concatBytes(chunks), false, true), final);
                },
            });
        } catch (e) {
            N.queueTask(() => fail(e.message));
        }
    }
    postMessage(data, transfer) {
        if (!arguments.length) throw new TypeError('postMessage: 1 argument required');
        const [tree, cids, buffers] = encodeMessage(data, transfer);
        if (this._id) N.workerPost(this._id, tree, cids);
        detachAll(buffers);
    }
    terminate() {
        if (!this._id) return;
        N.workerTerminate(this._id);
        workersById.delete(this._id);
        this._id = 0;
    }
}
defineHandlers(Worker.prototype, ['message', 'messageerror', 'error']);
tag(Worker);
defGlobal('Worker', Worker);
/* From worker.c. kind 0: a message from worker id; 1: a message for a
 * port's channel id; 2: an uncaught error in the worker; 3: it ended;
 * with 16 added a message could not be read. */
P.workerDeliver = function (kind, id, a, b, c) {
    if (kind === 1) { P.portDeliver(id, a, b); return; }
    const w = workersById.get(id);
    if (!w) return;
    if (kind & 16) { fire(w, new MessageEvent('messageerror', {})); return; }
    if (kind === 0) {
        const ports = adoptEnds(b);
        let data;
        try { data = scDecode(a, ports); } catch (e) { fire(w, new MessageEvent('messageerror', {})); return; }
        fire(w, new MessageEvent('message', { data, ports }));
    } else if (kind === 2) {
        fire(w, new ErrorEvent('error', { cancelable: true, message: a, filename: b, lineno: c }));
    } else if (kind === 3) {
        workersById.delete(id);
        w._id = 0;
    }
};
