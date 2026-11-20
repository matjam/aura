
/* ---------------- Blob, File, FileReader ---------------- */

const bytesOf = (part) => {
    if (part instanceof Blob) return part._b;
    if (part instanceof ArrayBuffer) return new Uint8Array(part.slice(0));
    if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer.slice(part.byteOffset, part.byteOffset + part.byteLength));
    return N.utf8Encode(str(part).toWellFormed());
};
const concatBytes = (list) => {
    let n = 0;
    for (const b of list) n += b.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const b of list) { out.set(b, o); o += b.length; }
    return out;
};
class Blob {
    constructor(parts = [], opts = {}) {
        if (parts === null || typeof parts !== 'object' || typeof parts[Symbol.iterator] !== 'function')
            throw new TypeError('Blob: the parts are not a sequence');
        const list = [];
        for (const p of parts) list.push(bytesOf(p));
        this._b = concatBytes(list);
        const t = opts && opts.type !== undefined ? str(opts.type) : '';
        this._t = /^[\x20-\x7e]*$/.test(t) ? t.toLowerCase() : '';
    }
    get size() { return this._b.length; }
    get type() { return this._t; }
    slice(start = 0, end = this._b.length, type = '') {
        const n = this._b.length;
        const rel = (x) => (x < 0 ? Math.max(n + x, 0) : Math.min(x, n));
        const b = new Blob([], { type });
        b._b = this._b.slice(rel(start | 0), rel(end | 0));
        return b;
    }
    text() { return Promise.resolve(N.utf8Decode(this._b, false, false)); }
    arrayBuffer() { return Promise.resolve(this._b.slice().buffer); }
    bytes() { return Promise.resolve(this._b.slice()); }
    stream() { return streams().streamFromBytes(this._b); }
}
tag(Blob);
defGlobal('Blob', Blob);
class File extends Blob {
    constructor(parts, name, opts = {}) {
        super(parts, opts);
        if (arguments.length < 2) throw new TypeError('File: 2 arguments required');
        this._n = str(name);
        this._m = opts && opts.lastModified !== undefined ? Number(opts.lastModified) : Date.now();
    }
    get name() { return this._n; }
    get lastModified() { return this._m; }
    get webkitRelativePath() { return ''; }
}
tag(File);
defGlobal('File', File);
/* FileList: what <input type=file>.files holds (always empty: Aura has no
 * file picker for pages yet). */
class FileList {
    constructor() { throw new TypeError('Illegal constructor'); }
    item(i) { return this[i] || null; }
    *[Symbol.iterator]() { for (let i = 0; i < this.length; i++) yield this[i]; }
}
tag(FileList);
defGlobal('FileList', FileList);
const emptyFileList = () => Object.freeze(Object.create(FileList.prototype, { length: { value: 0, enumerable: true } }));

class FileReader extends EventTarget {
    constructor() { super(); this._state = 0; this._result = null; this._error = null; }
    get readyState() { return this._state; }
    get result() { return this._result; }
    get error() { return this._error; }
    _read(blob, how, enc) {
        if (!(blob instanceof Blob)) throw new TypeError('FileReader: the argument is not a Blob');
        if (this._state === 1) throw domErr('A read is in progress', 'InvalidStateError');
        this._state = 1;
        this._result = null;
        N.queueTask(() => {
            if (this._state !== 1) return;
            fire(this, new ProgressEvent('loadstart', { total: blob.size }));
            const b = blob._b;
            if (how === 0) this._result = b.slice().buffer;
            else if (how === 1) this._result = new TextDecoder(enc || 'utf-8').decode(b);
            else if (how === 2) this._result = 'data:' + (blob.type || 'application/octet-stream') + ';base64,' + bytesToBase64(b);
            else { let s = ''; for (const x of b) s += String.fromCharCode(x); this._result = s; }
            this._state = 2;
            fire(this, new ProgressEvent('progress', { lengthComputable: true, loaded: b.length, total: b.length }));
            fire(this, new ProgressEvent('load', { lengthComputable: true, loaded: b.length, total: b.length }));
            fire(this, new ProgressEvent('loadend', { lengthComputable: true, loaded: b.length, total: b.length }));
        });
    }
    readAsArrayBuffer(b) { this._read(b, 0); }
    readAsText(b, enc) { this._read(b, 1, enc); }
    readAsDataURL(b) { this._read(b, 2); }
    readAsBinaryString(b) { this._read(b, 3); }
    abort() {
        if (this._state !== 1) return;
        this._state = 2;
        this._result = null;
        fire(this, new ProgressEvent('abort'));
        fire(this, new ProgressEvent('loadend'));
    }
}
for (const [k, v] of [['EMPTY', 0], ['LOADING', 1], ['DONE', 2]]) { FileReader[k] = v; FileReader.prototype[k] = v; }
defineHandlers(FileReader.prototype, ['loadstart', 'progress', 'load', 'abort', 'error', 'loadend']);
tag(FileReader);
defGlobal('FileReader', FileReader);

function bytesToBase64(b) {
    let s = '';
    for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode.apply(null, b.subarray(i, i + 0x8000));
    return btoa(s);
}

/* blob: URLs, per document */
const blobUrls = {
    map: new Map(),
    seq: 0,
    create(blob) {
        if (!(blob instanceof Blob)) throw new TypeError('createObjectURL: the argument is not a Blob');
        const o = N.docUrl(2);
        const u = 'blob:' + (o === 'null' || o === 'file://' ? 'null' : o) + '/' + (++this.seq).toString(16).padStart(8, '0') +
            '-0000-4000-8000-' + Math.floor(Math.random() * 0xffffffffffff).toString(16).padStart(12, '0');
        this.map.set(u, blob);
        return u;
    },
    revoke(u) { this.map.delete(u); },
    get(u) { return this.map.get(u.replace(/#.*$/, '')); },
};

/* ---------------- FormData ---------------- */

class FormData {
    constructor(form, submitter) {
        this._l = [];
        if (form !== undefined) {
            if (!(isElement(form) && form.localName === 'form')) throw new TypeError('FormData: the argument is not a form');
            for (const [k, v] of formDataSet(form, submitter || null)) this._l.push([k, v]);
        }
    }
    _entry(name, value, filename) {
        name = str(name);
        if (value instanceof Blob) {
            if (!(value instanceof File) || filename !== undefined) value = new File([value], filename !== undefined ? str(filename) : value instanceof File ? value.name : 'blob', { type: value.type });
            return [name, value];
        }
        return [name, str(value)];
    }
    append(n, v, f) { this._l.push(this._entry(n, v, f)); }
    delete(n) { n = str(n); this._l = this._l.filter((p) => p[0] !== n); }
    get(n) { n = str(n); const p = this._l.find((x) => x[0] === n); return p ? p[1] : null; }
    getAll(n) { n = str(n); return this._l.filter((x) => x[0] === n).map((x) => x[1]); }
    has(n) { n = str(n); return this._l.some((x) => x[0] === n); }
    set(n, v, f) {
        const e = this._entry(n, v, f);
        const i = this._l.findIndex((x) => x[0] === e[0]);
        if (i < 0) this._l.push(e);
        else { this._l[i] = e; this._l = this._l.filter((x, j) => j <= i || x[0] !== e[0]); }
    }
    forEach(fn, thisArg) { for (const [k, v] of this._l.slice()) fn.call(thisArg, v, k, this); }
    keys() { return this._l.map((p) => p[0])[Symbol.iterator](); }
    values() { return this._l.map((p) => p[1])[Symbol.iterator](); }
    entries() { return this._l.map((p) => p.slice())[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
}
tag(FormData);
defGlobal('FormData', FormData);

/* ---------------- Headers ---------------- */

const headerName = (n) => {
    n = str(n);
    if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(n)) throw new TypeError('Invalid header name: ' + n);
    return n.toLowerCase();
};
const headerValue = (v) => {
    v = str(v).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
    if (/[\0\r\n]/.test(v)) throw new TypeError('Invalid header value');
    return v;
};
/* The forbidden request headers (the engine drops them again: fetch.c). */
const forbiddenRequest = new Set(['accept-charset', 'accept-encoding', 'access-control-request-headers',
    'access-control-request-method', 'connection', 'content-length', 'cookie', 'cookie2', 'date', 'dnt', 'expect', 'host',
    'keep-alive', 'origin', 'referer', 'set-cookie', 'te', 'trailer', 'transfer-encoding', 'upgrade', 'via']);
const isForbiddenRequest = (n, v) => forbiddenRequest.has(n) || n.startsWith('proxy-') || n.startsWith('sec-') ||
    (/^x-(http-method|http-method-override|method-override)$/.test(n) && /(^|,)\s*(connect|trace|track)\s*(,|$)/i.test(v || ''));
const noCorsSafe = (n, v) => {
    if (v.length > 128) return false;
    if (n === 'accept') return !/[\0-\x08\x0a-\x1f"():<>?@[\\\]{}\x7f]/.test(v);
    if (n === 'accept-language' || n === 'content-language') return /^[0-9A-Za-z *,\-.;=]*$/.test(v);
    if (n === 'content-type') return !/[\0-\x08\x0a-\x1f"():<>?@[\\\]{}\x7f]/.test(v) &&
        /^\s*(application\/x-www-form-urlencoded|multipart\/form-data|text\/plain)\s*(;|$)/i.test(v);
    return false;
};
class Headers {
    constructor(init) {
        this._l = [];
        this._guard = 'none';
        if (init !== undefined && init !== null) this._fill(init);
    }
    _fill(init) {
        if (init instanceof Headers) { for (const [k, v] of init._l) this.append(k, v); return; }
        if (typeof init !== 'object') throw new TypeError('Headers: invalid init');
        if (typeof init[Symbol.iterator] === 'function') {
            for (const pair of init) {
                const a = Array.from(pair);
                if (a.length !== 2) throw new TypeError('Headers: each pair must have two items');
                this.append(a[0], a[1]);
            }
        } else for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    /* True when the guard refuses the header (it is then left out). */
    _check(n, v) {
        if (this._guard === 'immutable') throw new TypeError('The headers are immutable');
        if (this._guard === 'request' || this._guard === 'request-no-cors') { if (isForbiddenRequest(n, v)) return true; }
        if (this._guard === 'request-no-cors' && v !== undefined) {
            const cur = this.get(n);
            if (!noCorsSafe(n, cur === null ? v : cur + ', ' + v)) return true;
        }
        if (this._guard === 'response' && (n === 'set-cookie' || n === 'set-cookie2')) return true;
        return false;
    }
    append(n, v) {
        n = headerName(n); v = headerValue(v);
        if (this._check(n, v)) return;
        this._l.push([n, v]);
    }
    delete(n) {
        n = headerName(n);
        if (this._check(n)) return;
        if (this._guard === 'request-no-cors' && !['accept', 'accept-language', 'content-language', 'content-type'].includes(n)) return;
        this._l = this._l.filter((p) => p[0] !== n);
    }
    get(n) {
        n = headerName(n);
        const vs = this._l.filter((p) => p[0] === n).map((p) => p[1]);
        return vs.length ? vs.join(', ') : null;
    }
    getSetCookie() { return this._l.filter((p) => p[0] === 'set-cookie').map((p) => p[1]); }
    has(n) { n = headerName(n); return this._l.some((p) => p[0] === n); }
    set(n, v) {
        n = headerName(n); v = headerValue(v);
        if (this._guard === 'immutable') throw new TypeError('The headers are immutable');
        if ((this._guard === 'request' || this._guard === 'request-no-cors') && isForbiddenRequest(n, v)) return;
        if (this._guard === 'request-no-cors' && !noCorsSafe(n, v)) return;
        if (this._guard === 'response' && (n === 'set-cookie' || n === 'set-cookie2')) return;
        const i = this._l.findIndex((p) => p[0] === n);
        if (i < 0) this._l.push([n, v]);
        else { this._l[i][1] = v; this._l = this._l.filter((p, j) => j <= i || p[0] !== n); }
    }
    _sorted() {
        const names = [...new Set(this._l.map((p) => p[0]))].sort();
        const out = [];
        for (const n of names) {
            if (n === 'set-cookie') for (const v of this.getSetCookie()) out.push([n, v]);
            else out.push([n, this.get(n)]);
        }
        return out;
    }
    forEach(fn, thisArg) { for (const [k, v] of this._sorted()) fn.call(thisArg, v, k, this); }
    keys() { return this._sorted().map((p) => p[0])[Symbol.iterator](); }
    values() { return this._sorted().map((p) => p[1])[Symbol.iterator](); }
    entries() { return this._sorted()[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
}
tag(Headers);
defGlobal('Headers', Headers);

/* ---------------- Body, Request, Response ---------------- */

/* A ReadableStream body (only possible once streams.js is loaded: a page
 * that never touched a stream class cannot hold one). */
const isStreamBody = (body) => streamsLib !== null && streamsLib.isReadable(body);
const streamUnusable = (s) => { const st = streamsLib.streamState(s); return st.locked || st.disturbed; };
/* Extracts a body: [bytes or null, content type or null, stream or
 * undefined]. A ReadableStream comes back as itself (allowStream), or is
 * taken as a string (XMLHttpRequest and sendBeacon, whose body types do
 * not include streams). */
function extractBody(body, allowStream) {
    if (body === null || body === undefined) return [null, null];
    if (allowStream && isStreamBody(body)) {
        if (streamUnusable(body)) throw new TypeError('The body stream is locked or has been read');
        return [null, null, body];
    }
    if (typeof body === 'string') return [N.utf8Encode(body.toWellFormed()), 'text/plain;charset=UTF-8'];
    if (body instanceof URLSearchParams) return [N.utf8Encode(body.toString()), 'application/x-www-form-urlencoded;charset=UTF-8'];
    if (body instanceof Blob) return [body._b, body.type || null];
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return [bytesOf(body), null];
    if (body instanceof FormData) {
        const boundary = '----AuraFormBoundary' + Math.floor(Math.random() * 1e16).toString(36);
        const parts = [];
        const esc = (s) => s.replace(/\n/g, '%0A').replace(/\r/g, '%0D').replace(/"/g, '%22');
        for (const [k, v] of body._l) {
            let head = '--' + boundary + '\r\nContent-Disposition: form-data; name="' + esc(k) + '"';
            if (v instanceof File) {
                head += '; filename="' + esc(v.name) + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream') + '\r\n\r\n';
                parts.push(N.utf8Encode(head), v._b, N.utf8Encode('\r\n'));
            } else {
                parts.push(N.utf8Encode(head + '\r\n\r\n' + v.replace(/\r?\n|\r/g, '\r\n') + '\r\n'));
            }
        }
        parts.push(N.utf8Encode('--' + boundary + '--\r\n'));
        return [concatBytes(parts), 'multipart/form-data; boundary=' + boundary];
    }
    return [N.utf8Encode(str(body).toWellFormed()), 'text/plain;charset=UTF-8'];
}
/* Parses a multipart/form-data body (bytes) with its boundary. */
function parseMultipart(bytes, boundary) {
    const fd = new FormData();
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    const delim = '--' + boundary;
    let pos = s.indexOf(delim);
    if (pos < 0) throw new TypeError('The body is not valid multipart/form-data');
    const latin = (t) => { const b = new Uint8Array(t.length); for (let i = 0; i < t.length; i++) b[i] = t.charCodeAt(i); return b; };
    for (;;) {
        pos += delim.length;
        if (s.startsWith('--', pos)) break;
        const hs = s.indexOf('\r\n', pos) + 2;
        const he = s.indexOf('\r\n\r\n', hs - 2);
        if (hs < 2 || he < 0) throw new TypeError('The body is not valid multipart/form-data');
        const next = s.indexOf('\r\n' + delim, he + 4);
        if (next < 0) throw new TypeError('The body is not valid multipart/form-data');
        const headers = s.slice(hs, he);
        const cd = /^content-disposition:\s*form-data\s*;(.*)$/im.exec(headers);
        if (!cd) throw new TypeError('The body is not valid multipart/form-data');
        const nm = /(?:^|;)\s*name="([^"]*)"/i.exec(cd[1]);
        const fnm = /(?:^|;)\s*filename="([^"]*)"/i.exec(cd[1]);
        const ct = /^content-type:\s*(.*)$/im.exec(headers);
        const un = (t) => N.utf8Decode(latin(t.replace(/%0A/g, '\n').replace(/%0D/g, '\r').replace(/%22/g, '"')), false, false);
        const raw = latin(s.slice(he + 4, next));
        if (!nm) throw new TypeError('The body is not valid multipart/form-data');
        if (fnm) fd.append(un(nm[1]), new File([raw], un(fnm[1]), { type: ct ? ct[1].trim() : '' }));
        else fd.append(un(nm[1]), N.utf8Decode(raw, false, false));
        pos = next + 2;
    }
    return fd;
}
/* ---- A network body as it arrives ----
 * A response's body from the network is a feed until script reads it:
 * the pieces are kept in order, then handed to whichever reader comes
 * (text() and the other methods collect them; Response.body turns the
 * feed into a ReadableStream, streams.js bodyFromFeed, which also pauses
 * the network while the stream holds more than its reader has taken). A
 * clone gets a feed of its own. A source is the network side of a
 * response: { feeds, ended, abort, pause, paused, update }. A feed is
 * { src, chunks, state (0 open, 1 ended, 2 failed), err, sink, waiter }.
 * Plain records and four functions, so that the prelude stays small. */
const newFeed = (src) => {
    const f = { src, chunks: [], state: 0, err: null, sink: null, waiter: null };
    src.feeds.push(f);
    return f;
};
/* The network gave a piece (kind 0), ended (1) or failed (2). */
function feedEvent(src, kind, v) {
    if (kind) src.ended = true;
    src.feeds.forEach((f, i) => {
        if (kind === 0) {
            const b = i ? v.slice() : v;
            if (f.sink) f.sink.chunk(b); else f.chunks.push(b);
            return;
        }
        f.state = kind;
        f.err = v;
        if (f.sink) { if (kind === 1) f.sink.close(); else f.sink.error(v); return; }
        const w = f.waiter;
        f.waiter = null;
        if (w && kind === 1) w[0](concatBytes(f.chunks));
        else if (w) w[1](v);
        if (w || kind === 2) f.chunks = [];
    });
    if (kind === 0 && src.update) src.update(src);
}
/* text(), json(), ...: the whole body */
function feedCollect(f) {
    if (f.state === 1) { const all = concatBytes(f.chunks); f.chunks = []; return Promise.resolve(all); }
    if (f.state === 2) return Promise.reject(f.err);
    return new Promise((a, b) => { f.waiter = [a, b]; });
}
function feedFork(f) {
    const g = newFeed(f.src);
    g.chunks = f.chunks.map((c) => c.slice());
    g.state = f.state;
    g.err = f.err;
    return g;
}
const nullBodyStatus = (s) => s === 101 || s === 103 || s === 204 || s === 205 || s === 304;

/* A Request's or Response's body is one of: none (_hasBody false), bytes
 * held in memory (_bytes), a network feed (_feed) or a ReadableStream
 * (_stream, made when script asks for .body or passes a stream). */
const bodyMixin = {
    get body() {
        if (!this._hasBody) return null;
        if (this._stream) return this._stream;
        const lib = streams();
        if (this._used) this._stream = lib.usedStream();
        else if (this._feed) this._stream = lib.bodyFromFeed(this._feed);
        else this._stream = lib.streamFromBytes(this._bytes || new Uint8Array(0));
        this._feed = null;
        this._bytes = null;
        return this._stream;
    },
    get bodyUsed() {
        if (this._stream) return streamsLib.streamState(this._stream).disturbed;
        return !!this._used;
    },
    _unusable() { return this._stream ? streamUnusable(this._stream) : !!this._used; },
    _consume() {
        if (this._unusable()) return Promise.reject(new TypeError('The body has already been read'));
        if (!this._hasBody) return Promise.resolve(new Uint8Array(0));
        if (this._stream) return streamsLib.readAll(this._stream);
        this._used = true;
        if (this._feed) { const f = this._feed; this._feed = null; return feedCollect(f); }
        const b = this._bytes || new Uint8Array(0);
        this._bytes = null;
        return Promise.resolve(b);
    },
    arrayBuffer() { return this._consume().then((b) => b.slice().buffer); },
    bytes() { return this._consume().then((b) => b.slice()); },
    text() { return this._consume().then((b) => N.utf8Decode(b, false, false)); },
    json() { return this.text().then((t) => JSON.parse(t)); },
    blob() { return this._consume().then((b) => { const x = new Blob([], { type: (this.headers.get('content-type') || '') }); x._b = b; return x; }); },
    formData() {
        return this._consume().then((b) => {
            const ct = this.headers.get('content-type') || '';
            const m = /^multipart\/form-data\s*;.*?boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(ct);
            if (m) return parseMultipart(b, m[1] || m[2]);
            if (!/^application\/x-www-form-urlencoded/i.test(ct)) throw new TypeError('The body is not a form');
            const fd = new FormData();
            for (const [k, v] of new URLSearchParams(N.utf8Decode(b, false, false))) fd.append(k, v);
            return fd;
        });
    },
};

const modeNum = { cors: 0, 'no-cors': 1, 'same-origin': 2, navigate: 3 };
const credNum = { omit: 0, 'same-origin': 1, include: 2 };
const cacheNum = { default: 0, 'no-store': 1, reload: 2, 'no-cache': 3, 'force-cache': 4, 'only-if-cached': 5 };
const redirectNum = { follow: 0, error: 1, manual: 2 };
const policyNum = { '': 0, 'no-referrer': 1, 'no-referrer-when-downgrade': 2, 'same-origin': 3, origin: 4,
    'strict-origin': 5, 'origin-when-cross-origin': 6, 'strict-origin-when-cross-origin': 7, 'unsafe-url': 8 };
const methodToken = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const normalMethod = (m) => (/^(DELETE|GET|HEAD|OPTIONS|POST|PUT|PATCH)$/i.test(m) ? m.toUpperCase() : m);
const enumOf = (table, v, what) => { v = str(v); if (!(v in table)) throw new TypeError('Invalid ' + what + ': ' + v); return v; };
class Request {
    constructor(input, init = {}) {
        if (!arguments.length) throw new TypeError('Request: 1 argument required');
        const src = input instanceof Request ? input : null;
        let url;
        if (src) url = src._url;
        else {
            try { url = new URL(str(input), N.docUrl(1)); } catch (e) { throw new TypeError('Invalid request URL: ' + str(input)); }
            if (url.username || url.password) throw new TypeError('A request URL must not include credentials');
            url = url.href;
        }
        if (init === null || init === undefined) init = {};
        if (typeof init !== 'object') throw new TypeError('Request: the init is not an object');
        const has = (k) => init[k] !== undefined;
        if (has('window') && init.window !== null) throw new TypeError('Request: window must be null');
        let method = src ? src._method : 'GET';
        if (has('method')) {
            method = str(init.method);
            if (!methodToken.test(method)) throw new TypeError('Invalid method');
            if (/^(CONNECT|TRACE|TRACK)$/i.test(method)) throw new TypeError('Forbidden method: ' + method);
            method = normalMethod(method);
        }
        this._url = url;
        this._method = method;
        this._mode = has('mode') ? enumOf(modeNum, init.mode, 'mode') : src ? src._mode : 'cors';
        if (has('mode') && this._mode === 'navigate') throw new TypeError('Invalid mode: navigate');
        /* a request made from a navigation request starts over as same-origin */
        if (src && src._mode === 'navigate' && Object.keys(init).length) this._mode = 'same-origin';
        this._cred = has('credentials') ? enumOf(credNum, init.credentials, 'credentials') : src ? src._cred : 'same-origin';
        this._cache = has('cache') ? enumOf(cacheNum, init.cache, 'cache') : src ? src._cache : 'default';
        if (this._cache === 'only-if-cached' && this._mode !== 'same-origin') throw new TypeError('only-if-cached needs mode same-origin');
        this._redirect = has('redirect') ? enumOf(redirectNum, init.redirect, 'redirect') : src ? src._redirect : 'follow';
        this._integrity = has('integrity') ? str(init.integrity) : src ? src._integrity : '';
        this._keepalive = has('keepalive') ? !!init.keepalive : src ? src._keepalive : false;
        this._policy = has('referrerPolicy') ? enumOf(policyNum, init.referrerPolicy, 'referrerPolicy') : src ? src._policy : '';
        this._referrer = src ? src._referrer : 'about:client';
        if (has('referrer')) {
            const r = str(init.referrer);
            if (r === '') this._referrer = '';
            else {
                let ru;
                try { ru = new URL(r, N.docUrl(1)); } catch (e) { throw new TypeError('Invalid referrer'); }
                this._referrer = ru.href === 'about:client' || ru.origin !== N.docUrl(2) ? 'about:client' : ru.href;
            }
        }
        if (has('signal') && init.signal !== null && !(init.signal instanceof AbortSignal)) throw new TypeError('Request: signal is not an AbortSignal');
        const parent = has('signal') ? init.signal : src ? src._signal : null;
        this._signal = AbortSignal.any(parent ? [parent] : []);
        if (this._mode === 'no-cors' && !/^(GET|HEAD|POST)$/.test(method)) throw new TypeError('A no-cors request must be GET, HEAD or POST');
        this.headers = new Headers();
        this.headers._guard = this._mode === 'no-cors' ? 'request-no-cors' : 'request';
        const hinit = has('headers') ? init.headers : src ? src.headers : undefined;
        if (hinit !== undefined) this.headers._fill(hinit);
        const body = has('body') && init.body !== null ? init.body : null;
        if (src && src._hasBody && body === null && src._unusable()) throw new TypeError('The source request body has been used');
        if ((body !== null || (src && src._hasBody)) && (method === 'GET' || method === 'HEAD'))
            throw new TypeError('A GET or HEAD request cannot have a body');
        this._bytes = null;
        this._stream = null;
        this._hasBody = false;
        if (body !== null) {
            const [bytes, type, stream] = extractBody(body, true);
            if (stream) {
                if (this._keepalive) throw new TypeError('A keepalive request cannot have a stream body');
                if (!has('duplex') || str(init.duplex) !== 'half') throw new TypeError('A stream body needs duplex: "half"');
                this._stream = stream;
            } else this._bytes = bytes;
            this._hasBody = true;
            if (type && !this.headers.has('content-type')) this.headers.append('content-type', type);
        } else if (src && src._hasBody) {
            this._hasBody = true;
            if (src._stream) {
                /* the new request takes the stream; the source's is used up */
                this._stream = src._stream;
                src._stream = streams().usedStream();
            } else this._bytes = src._bytes;
        }
    }
    get url() { return this._url; }
    get method() { return this._method; }
    get mode() { return this._mode; }
    get credentials() { return this._cred; }
    get cache() { return this._cache; }
    get redirect() { return this._redirect; }
    get signal() { return this._signal; }
    get destination() { return ''; }
    get referrer() { return this._referrer; }
    get referrerPolicy() { return this._policy; }
    get integrity() { return this._integrity; }
    get keepalive() { return this._keepalive; }
    get isHistoryNavigation() { return false; }
    get isReloadNavigation() { return false; }
    get duplex() { return 'half'; }
    clone() {
        if (this._unusable()) throw new TypeError('The body has been used');
        const r = Object.create(Request.prototype);
        Object.assign(r, this);
        r.headers = new Headers(this.headers);
        r.headers._guard = this.headers._guard;
        r._signal = AbortSignal.any([this._signal]);
        if (this._stream) {
            const [a, b] = streamsLib.tee(this._stream);
            this._stream = a;
            r._stream = b;
        }
        return r;
    }
}
defProps(Request.prototype, bodyMixin);
tag(Request);
defGlobal('Request', Request);

const redirectStatus = new Set([301, 302, 303, 307, 308]);
class Response {
    constructor(body = null, init = {}) {
        if (init === null || init === undefined) init = {};
        const status = init.status !== undefined ? Number(init.status) : 200;
        if (!(status >= 200 && status <= 599) || status !== (status | 0)) throw new RangeError('Invalid status');
        this._status = status;
        this._statusText = init.statusText !== undefined ? str(init.statusText) : '';
        if (/[^\t\x20-\x7e\x80-\xff]/.test(this._statusText)) throw new TypeError('Invalid status text');
        this.headers = new Headers();
        this.headers._guard = 'response';
        if (init.headers !== undefined) this.headers._fill(init.headers);
        this._type = 'default';
        this._url = '';
        this._redirected = false;
        this._bytes = null;
        this._stream = null;
        this._feed = null;
        this._hasBody = false;
        if (body !== null && body !== undefined) {
            if (nullBodyStatus(status)) throw new TypeError('This status cannot have a body');
            const [bytes, type, stream] = extractBody(body, true);
            if (stream) this._stream = stream; else this._bytes = bytes;
            this._hasBody = true;
            if (type && !this.headers.has('content-type')) this.headers.append('content-type', type);
        }
    }
    static error() { const r = new Response(); r._type = 'error'; r._status = 0; r.headers._guard = 'immutable'; return r; }
    static redirect(url, status = 302) {
        let u;
        try { u = new URL(str(url), N.docUrl(1)).href; } catch (e) { throw new TypeError('Invalid URL'); }
        if (!redirectStatus.has(status)) throw new RangeError('Invalid redirect status');
        const r = new Response(null, { status });
        r.headers.set('location', u);
        r.headers._guard = 'immutable';
        return r;
    }
    static json(data, init = {}) {
        const s = JSON.stringify(data);
        if (s === undefined) throw new TypeError('The value cannot be serialized as JSON');
        const r = new Response(s, init);
        if (!(init && init.headers !== undefined && new Headers(init.headers).has('content-type'))) r.headers.set('content-type', 'application/json');
        return r;
    }
    get type() { return this._type; }
    get url() { return this._url; }
    get redirected() { return this._redirected; }
    get status() { return this._status; }
    get ok() { return this._status >= 200 && this._status <= 299; }
    get statusText() { return this._statusText; }
    clone() {
        if (this._unusable()) throw new TypeError('The body has been used');
        const r = Object.create(Response.prototype);
        Object.assign(r, this);
        r.headers = new Headers(this.headers);
        r.headers._guard = this.headers._guard;
        if (this._stream) {
            const [a, b] = streamsLib.tee(this._stream);
            this._stream = a;
            r._stream = b;
        } else if (this._feed) r._feed = feedFork(this._feed);
        return r;
    }
}
defProps(Response.prototype, bodyMixin);
tag(Response);
defGlobal('Response', Response);

/* Decodes a data: URL: [bytes, type] or null. */
function decodeDataUrl(url) {
    const m = /^data:([^,]*?)(;\s*base64)?,([^#]*)/is.exec(url);
    if (!m) return null;
    try {
        const raw = m[3].replace(/%([0-9a-f]{2})/gi, (x, h) => String.fromCharCode(parseInt(h, 16)));
        let body;
        if (m[2]) { const bin = atob(raw.replace(/[\t\n\f\r ]/g, '')); body = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) body[i] = bin.charCodeAt(i); }
        else { body = new Uint8Array(raw.length); for (let i = 0; i < raw.length; i++) body[i] = raw.charCodeAt(i) & 0xff; }
        let type = m[1].trim();
        if (!type || !/^[^/;\s]+\/[^/;\s]+/.test(type)) type = type.startsWith(';') ? 'text/plain' + type : 'text/plain;charset=US-ASCII';
        return [body, type];
    } catch (e) { return null; }
}
const typeNames = ['basic', 'cors', 'opaque', 'opaqueredirect'];

/* Starts a request through the engine (src/js/fetch.c applies the
 * same-origin policy and CORS and follows redirects). Calls
 * cb.head(status, text, headers, finalUrl, type, redirected), cb.data(bytes),
 * cb.done(ok, message). opts: cache, redirect, referrerPolicy (numbers),
 * referrer, preflight. Returns an abort function. data: and blob: URLs
 * are answered here. */
function startFetch(method, url, headerList, bytes, cred, mode, dest, cb, opts) {
    if (/^data:/i.test(url)) {
        const d = decodeDataUrl(url);
        let live = true;
        N.queueTask(() => {
            if (!live) return;
            if (!d) { cb.done(false, 'invalid data: URL'); return; }
            cb.head(200, 'OK', [['content-type', d[1]]], url, 'basic', false);
            cb.data(d[0]);
            cb.done(true);
        });
        return () => { live = false; };
    }
    if (/^blob:/i.test(url)) {
        const blob = blobUrls.get(url);
        let live = true;
        N.queueTask(() => {
            if (!live) return;
            if (!blob || method !== 'GET') { cb.done(false, 'the blob URL is not valid'); return; }
            cb.head(200, 'OK', [['content-type', blob.type], ['content-length', String(blob.size)]], url, 'basic', false);
            cb.data(blob._b);
            cb.done(true);
        });
        return () => { live = false; };
    }
    let id = 0;
    let finished = false;
    id = N.fetchStart(method, url, headerList, bytes, cred, mode, dest, (msg) => {
        if (finished) return;
        switch (msg[0]) {
        case 'head': cb.head(msg[1], msg[2], msg[3], msg[4], typeNames[msg[5]], msg[6]); break;
        case 'data': cb.data(msg[1]); break;
        case 'done': finished = true; cb.done(true); break;
        case 'error': finished = true; cb.done(false, msg[1] || 'network error'); break;
        }
    }, opts);
    const abort = () => { if (!finished) { finished = true; N.fetchAbort(id); } };
    /* backpressure: stop or resume reading the response from the network */
    abort.pause = (paused) => { if (!finished) N.fetchPause(id, paused); };
    /* a WebSocket's connection, after its head: queue bytes to send */
    abort.send = (bytes) => !finished && N.fetchSend(id, bytes);
    return abort;
}
/* A request's options for startFetch. */
const fetchOpts = (req) => ({
    cache: cacheNum[req._cache], redirect: redirectNum[req._redirect], referrerPolicy: policyNum[req._policy],
    referrer: req._referrer === 'about:client' ? undefined : req._referrer,
});

/* Subresource integrity: whether bytes match the metadata ("sha256-..."
 * entries; the strongest algorithm present decides; metadata without a
 * known algorithm allows everything). */
function integrityOk(meta, bytes) {
    const items = [];
    for (const tok of str(meta).split(/[\t\n\f\r ]+/)) {
        const m = /^(sha256|sha384|sha512)-([A-Za-z0-9+/_-]+={0,2})(\?.*)?$/i.exec(tok);
        if (m) items.push([m[1].toLowerCase(), m[2].replace(/-/g, '+').replace(/_/g, '/')]);
    }
    if (!items.length) return true;
    const best = ['sha512', 'sha384', 'sha256'].find((a) => items.some((i) => i[0] === a));
    const got = bytesToBase64(new Uint8Array(N.digest('SHA-' + best.slice(3), bytes)));
    return items.some((i) => i[0] === best && i[1] === got);
}

defGlobal('fetch', function fetch(input, init) {
    let req;
    try {
        req = new Request(input, init);
        if (req._hasBody && req._unusable()) throw new TypeError('Failed to fetch: the request body has been used');
    } catch (e) {
        return Promise.reject(e);
    }
    /* A stream body is read whole first and sent with a length: requests
     * are not streamed (see docs/known-gaps.md). */
    if (req._stream) return streamsLib.readAll(req._stream).then((bytes) => fetchRequest(req, bytes));
    const bytes = req._bytes;
    if (req._hasBody) { req._used = true; req._bytes = null; }
    return fetchRequest(req, bytes);
});
/* fetch() for a Request whose body is bytes (or none). The response is
 * given as soon as its head arrives; its body follows as a feed. */
function fetchRequest(req, bodyBytes) {
    return new Promise((resolve, reject) => {
        const signal = req._signal;
        if (signal.aborted) { reject(signal.reason); return; }
        const chunks = [];
        const src = { feeds: [], ended: false, abort: null, pause: null, paused: false, update: null };
        let response = null, pendingHead = null;
        let abort = () => {};
        const onAbort = () => {
            abort();
            const reason = signal.reason;
            if (!response) reject(reason); else feedEvent(src, 2, reason);
        };
        const makeResponse = (status, text, headers, finalUrl, type, redirected, bytes) => {
            const r = Object.create(Response.prototype);
            r._status = status;
            r._statusText = text;
            r.headers = new Headers();
            for (const [k, v] of headers) { try { r.headers.append(k, v); } catch (e) { /* malformed header: dropped */ } }
            r.headers._guard = 'immutable';
            r._type = type;
            r._url = finalUrl.replace(/#.*$/, '');
            r._redirected = !!redirected;
            r._bytes = null;
            r._stream = null;
            r._feed = null;
            r._hasBody = !nullBodyStatus(status) && req._method !== 'HEAD' && type !== 'opaque' && type !== 'opaqueredirect';
            if (bytes) r._bytes = bytes;
            else if (r._hasBody) r._feed = newFeed(src);
            response = r;
            resolve(r);
        };
        try {
            abort = startFetch(req._method, req._url.replace(/#.*$/, ''), req.headers._l, bodyBytes, credNum[req._cred], modeNum[req._mode], 0, {
                head(...a) {
                    /* with integrity metadata the response waits for its whole body */
                    if (req._integrity) pendingHead = a; else makeResponse(...a);
                },
                data(b) { if (req._integrity) chunks.push(b); else feedEvent(src, 0, b); },
                done(ok, msg) {
                    signal.removeEventListener('abort', onAbort);
                    if (ok) {
                        if (req._integrity) {
                            const all = concatBytes(chunks);
                            const opaque = pendingHead && (pendingHead[4] === 'opaque' || pendingHead[4] === 'opaqueredirect');
                            if (opaque || !integrityOk(req._integrity, all)) {
                                reject(new TypeError('Failed to fetch: the response does not match the integrity metadata'));
                                return;
                            }
                            makeResponse(...(pendingHead || [200, 'OK', [], req._url, 'basic', false]), all);
                            return;
                        }
                        if (!response) makeResponse(200, 'OK', [], req._url, 'basic', false);
                        feedEvent(src, 1, null);
                    } else {
                        const e = new TypeError('Failed to fetch: ' + (msg || 'network error'));
                        if (!response) reject(e); else feedEvent(src, 2, e);
                    }
                },
            }, fetchOpts(req));
        } catch (e) {
            reject(e);
            return;
        }
        src.abort = () => { signal.removeEventListener('abort', onAbort); abort(); };
        if (abort.pause) src.pause = abort.pause;
        signal.addEventListener('abort', onAbort);
    });
}

/* navigator.sendBeacon(url, data): a POST that outlives nothing here (a
 * document that goes cancels its requests), sent with credentials. */
function sendBeacon(url, data) {
    let u;
    try { u = new URL(str(url), N.docUrl(1)); } catch (e) { throw new TypeError('sendBeacon: invalid URL'); }
    if (u.protocol !== 'http:' && u.protocol !== 'https:') throw new TypeError('sendBeacon: the URL is not http or https');
    const [bytes, type] = extractBody(data === undefined ? null : data);
    if (bytes && bytes.length > 65536) return false;
    const headers = type ? [['content-type', type]] : [];
    const simple = !type || noCorsSafe('content-type', type);
    try {
        startFetch('POST', u.href, headers, bytes || new Uint8Array(0), 2, simple ? 1 : 0, 0, { head() {}, data() {}, done() {} });
    } catch (e) { return false; }
    return true;
}

/* ---------------- XMLHttpRequest ---------------- */

class XMLHttpRequestEventTarget extends EventTarget {}
defineHandlers(XMLHttpRequestEventTarget.prototype, ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend']);
class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}
const hasListeners = (t) => {
    const map = t[S.listeners];
    if (map) for (const list of map.values()) if (list.length) return true;
    return false;
};
const mimeOf = (ct) => {
    const m = /^\s*([^;\s]+)/.exec(ct || '');
    return m ? m[1].toLowerCase() : '';
};
const charsetOf = (ct) => {
    const m = /;\s*charset\s*=\s*(?:"([^"]*)"|([^;\s]*))/i.exec(ct || '');
    return m ? (m[1] || m[2] || '') : '';
};
class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
        super();
        this._state = 0;
        this._headers = [];
        this._resp = null;
        this._status = 0;
        this._statusText = '';
        this._respHeaders = [];
        this._type = '';
        this._chunks = [];
        this._bytes = null;
        this._abort = null;
        this._timeout = 0;
        this._timer = 0;
        this._cred = false;
        this._upload = new XMLHttpRequestUpload();
        this._url = '';
        this._send = false;
        this._error = false;
        this._mime = null;
        this._sync = false;
        this._finalUrl = '';
        this._loaded = 0;
        this._started = 0;
        this._uploading = false;
    }
    get readyState() { return this._state; }
    get status() { return this._error ? 0 : this._status; }
    get statusText() { return this._error ? '' : this._statusText; }
    get responseURL() { return this._error ? '' : this._finalUrl.replace(/#.*$/, ''); }
    get upload() { return this._upload; }
    get timeout() { return this._timeout; }
    set timeout(v) {
        if (this._sync && doc) throw domErr('A synchronous request of a document has no timeout', 'InvalidAccessError');
        this._timeout = v >>> 0;
        if (this._send && this._abort) this._arm();
    }
    get withCredentials() { return this._cred; }
    set withCredentials(v) {
        if ((this._state !== 0 && this._state !== 1) || this._send) throw domErr('The request has started', 'InvalidStateError');
        this._cred = !!v;
    }
    get responseType() { return this._type; }
    set responseType(v) {
        if (this._state === 3 || this._state === 4) throw domErr('The response is loading or done', 'InvalidStateError');
        if (this._sync && doc) throw domErr('A synchronous request of a document has no response type', 'InvalidAccessError');
        if (['', 'text', 'json', 'arraybuffer', 'blob', 'document'].includes(v)) this._type = v;
    }
    _setState(s) {
        this._state = s;
        fireSimple(this, 'readystatechange', false, false);
    }
    open(method, url, async = true, user, password) {
        if (arguments.length < 2) throw new TypeError('open: 2 arguments required');
        method = str(method);
        if (!methodToken.test(method)) throw domErr('Invalid method', 'SyntaxError');
        if (/^(CONNECT|TRACE|TRACK)$/i.test(method)) throw domErr('Forbidden method', 'SecurityError');
        method = normalMethod(method);
        let u;
        try { u = new URL(str(url), N.docUrl(1)); } catch (e) { throw domErr('Invalid URL', 'SyntaxError'); }
        if (arguments.length > 2 && !async && doc && (this._timeout || this._type !== ''))
            throw domErr('A synchronous request of a document has no timeout or response type', 'InvalidAccessError');
        if (user !== undefined && user !== null && u.hostname) u.username = str(user);
        if (password !== undefined && password !== null && u.hostname) u.password = str(password);
        if (this._abort) { this._abort(); this._abort = null; }
        this._disarm();
        this._method = method;
        this._url = u.href;
        this._sync = arguments.length > 2 && !async;
        this._headers = [];
        this._send = false;
        this._uploading = false;
        this._resp = null;
        this._chunks = [];
        this._bytes = null;
        this._status = 0;
        this._statusText = '';
        this._respHeaders = [];
        this._finalUrl = '';
        this._error = false;
        this._loaded = 0;
        if (this._state !== 1) this._setState(1);
    }
    setRequestHeader(n, v) {
        if (this._state !== 1 || this._send) throw domErr('The request is not opened', 'InvalidStateError');
        if (arguments.length < 2) throw new TypeError('setRequestHeader: 2 arguments required');
        n = str(n); v = str(v).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
        if (!methodToken.test(n)) throw domErr('Invalid header name', 'SyntaxError');
        if (/[\0\r\n]/.test(v)) throw domErr('Invalid header value', 'SyntaxError');
        if (isForbiddenRequest(n.toLowerCase(), v)) return;
        const h = this._headers.find((p) => p[0].toLowerCase() === n.toLowerCase());
        if (h) h[1] += ', ' + v; else this._headers.push([n, v]);
    }
    overrideMimeType(m) {
        if (this._state === 3 || this._state === 4) throw domErr('The response is loading or done', 'InvalidStateError');
        this._mime = /^\s*[^;\s/]+\/[^;\s/]+/.test(str(m)) ? str(m) : 'application/octet-stream';
    }
    _arm() {
        this._disarm();
        if (!this._timeout) return;
        const left = Math.max(0, this._timeout - (N.now() - this._started));
        this._timer = N.setTimeout(() => {
            this._timer = 0;
            if (!this._send) return;
            if (this._abort) { this._abort(); this._abort = null; }
            this._fail('timeout');
        }, left);
    }
    _disarm() { if (this._timer) { N.clearTimer(this._timer); this._timer = 0; } }
    send(body = null) {
        if (this._state !== 1 || this._send) throw domErr('The request is not opened', 'InvalidStateError');
        if (this._method === 'GET' || this._method === 'HEAD') body = null;
        let bytes = null;
        if (body !== null && body !== undefined) {
            let type;
            if (isNode(body) && body.nodeType === 9) {
                bytes = N.utf8Encode(body.documentElement ? body.documentElement.outerHTML : '');
                type = 'text/html;charset=UTF-8';
            } else [bytes, type] = extractBody(body);
            const own = this._headers.find((p) => p[0].toLowerCase() === 'content-type');
            if (!own) { if (type) this._headers.push(['Content-Type', type]); }
            else if (typeof body === 'string' || (isNode(body) && body.nodeType === 9))
                own[1] = own[1].replace(/(;\s*charset\s*=\s*)("[^"]*"|[^;\s]*)/i, '$1UTF-8');
        }
        this._uploading = !!bytes && bytes.length > 0;
        const preflight = hasListeners(this._upload);
        this._send = true;
        this._error = false;
        this._started = N.now();
        if (this._sync) { this._sendSync(bytes, preflight); return; }
        const xhr = this;
        const upTotal = bytes ? bytes.length : 0;
        fire(this, new ProgressEvent('loadstart', { loaded: 0, total: 0 }));
        if (this._state !== 1 || !this._send) return;
        if (this._uploading) fire(this._upload, new ProgressEvent('loadstart', { lengthComputable: true, loaded: 0, total: upTotal }));
        if (this._state !== 1 || !this._send) return;
        let total = 0;
        let progressAt = -1;
        try {
            this._abort = startFetch(this._method, this._url.replace(/#.*$/, ''), this._headers, bytes,
                this._cred ? 2 : 1, 0, 0, {
                    head(status, text, headers, finalUrl) {
                        if (xhr._uploading) {
                            xhr._uploading = false;
                            const pe = { lengthComputable: true, loaded: upTotal, total: upTotal };
                            fire(xhr._upload, new ProgressEvent('progress', pe));
                            fire(xhr._upload, new ProgressEvent('load', pe));
                            fire(xhr._upload, new ProgressEvent('loadend', pe));
                        }
                        xhr._status = status; xhr._statusText = text; xhr._respHeaders = headers; xhr._finalUrl = finalUrl;
                        const cl = headers.find((p) => p[0].toLowerCase() === 'content-length');
                        const ce = headers.find((p) => p[0].toLowerCase() === 'content-encoding');
                        total = cl && !ce && /^\d+$/.test(cl[1]) ? +cl[1] : 0;
                        xhr._setState(2);
                    },
                    data(b) {
                        if (xhr._state < 2) this.head(200, 'OK', [], xhr._url);
                        if (!xhr._send) return;
                        xhr._chunks.push(b);
                        xhr._loaded += b.length;
                        /* the standard's "roughly 50ms" between progress events
                         * (the first piece always reports) */
                        const now = N.now();
                        if (progressAt >= 0 && now - progressAt < 50) return;
                        progressAt = now;
                        if (xhr._state === 2) xhr._state = 3;
                        fireSimple(xhr, 'readystatechange', false, false);
                        if (!xhr._send) return;
                        fire(xhr, new ProgressEvent('progress', { lengthComputable: total > 0, loaded: xhr._loaded, total }));
                    },
                    done(ok, msg) {
                        xhr._disarm();
                        xhr._abort = null;
                        if (!xhr._send) return;
                        if (!ok) { xhr._fail('error'); return; }
                        if (xhr._state < 2) this.head(200, 'OK', [], xhr._url);
                        if (!xhr._send) return;
                        xhr._finish(total);
                    },
                }, { preflight });
        } catch (e) {
            this._abort = null;
            N.queueTask(() => { if (this._send) this._fail('error'); });
            return;
        }
        this._arm();
    }
    _finish(total) {
        const loaded = this._loaded;
        if (this._state < 3) this._state = 3;
        this._send = false;
        this._bytes = concatBytes(this._chunks);
        this._chunks = [];
        const pe = { lengthComputable: total > 0, loaded, total };
        if (!this._sync) fire(this, new ProgressEvent('progress', pe));
        this._setState(4);
        fire(this, new ProgressEvent('load', pe));
        fire(this, new ProgressEvent('loadend', pe));
    }
    /* A synchronous request. data: and blob: URLs are answered at once.
     * A worker on its own stack waits for the network while the page
     * runs (N.fetchSync). A document cannot wait: delivering network data
     * inside a script call would re-enter the parser and every loader, so
     * the request fails with NetworkError. */
    _sendSync(bytes, preflight) {
        const url = this._url.replace(/#.*$/, '');
        let r = null;
        if (/^data:/i.test(url)) {
            const d = decodeDataUrl(url);
            if (d) r = [200, [['content-type', d[1]]], d[0], url, 'OK'];
        } else if (/^blob:/i.test(url)) {
            const blob = blobUrls.get(url);
            if (blob && this._method === 'GET') r = [200, [['content-type', blob.type]], blob._b, url, 'OK'];
        } else {
            try { r = N.fetchSync(this._method, url, this._headers, bytes, this._cred ? 2 : 1, 0, 0, null, { preflight }); }
            catch (e) { r = null; }
        }
        if (!r) {
            this._error = true;
            this._send = false;
            this._state = 4;
            this._bytes = new Uint8Array(0);
            fireSimple(this, 'readystatechange', false, false);
            throw domErr('The synchronous request failed', 'NetworkError');
        }
        this._status = r[0]; this._respHeaders = r[1]; this._finalUrl = r[3]; this._statusText = r[4] || '';
        this._chunks = [r[2]];
        this._loaded = r[2].length;
        this._finish(r[2].length);
    }
    _fail(kind) {
        this._disarm();
        this._error = true;
        this._send = false;
        this._bytes = new Uint8Array(0);
        this._chunks = [];
        this._status = 0;
        this._respHeaders = [];
        this._setState(4);
        if (this._uploading) {
            this._uploading = false;
            fire(this._upload, new ProgressEvent(kind));
            fire(this._upload, new ProgressEvent('loadend'));
        }
        fire(this, new ProgressEvent(kind));
        fire(this, new ProgressEvent('loadend'));
    }
    abort() {
        if (this._abort) { this._abort(); this._abort = null; }
        if ((this._state === 1 && this._send) || this._state === 2 || this._state === 3) this._fail('abort');
        if (this._state === 4) { this._state = 0; this._send = false; }
    }
    getResponseHeader(n) {
        if (this._state < 2 || this._error) return null;
        n = str(n).toLowerCase();
        const vs = this._respHeaders.filter((p) => p[0].toLowerCase() === n && n !== 'set-cookie' && n !== 'set-cookie2').map((p) => p[1]);
        return vs.length ? vs.join(', ') : null;
    }
    getAllResponseHeaders() {
        if (this._state < 2 || this._error) return '';
        const h = new Headers();
        for (const [k, v] of this._respHeaders) { try { h.append(k, v); } catch (e) { /* dropped */ } }
        let s = '';
        for (const [k, v] of h) if (k !== 'set-cookie' && k !== 'set-cookie2') s += k + ': ' + v + '\r\n';
        return s;
    }
    /* The final type: overrideMimeType's, else Content-Type's. */
    _finalType() { return this._mime !== null ? this._mime : (this.getResponseHeader('content-type') || ''); }
    _text() {
        const b = this._bytes || concatBytes(this._chunks);
        let enc = charsetOf(this._finalType());
        if (b.length >= 3 && b[0] === 0xef && b[1] === 0xbb && b[2] === 0xbf) enc = 'utf-8';
        else if (b.length >= 2 && b[0] === 0xff && b[1] === 0xfe) enc = 'utf-16le';
        else if (b.length >= 2 && b[0] === 0xfe && b[1] === 0xff) enc = 'utf-16be';
        if (enc && !/^utf-?8$/i.test(enc)) { try { return new TextDecoder(enc).decode(b); } catch (e) { /* unknown label: UTF-8 */ } }
        return N.utf8Decode(b, false, false);
    }
    get responseText() {
        if (this._type !== '' && this._type !== 'text') throw domErr('responseText needs responseType "" or "text"', 'InvalidStateError');
        if (this._state < 3 || this._error) return '';
        return this._text();
    }
    _document() {
        if (this._state !== 4 || this._error) return null;
        if (this._doc !== undefined) return this._doc;
        this._doc = null;
        const mime = mimeOf(this._finalType());
        /* HTML only, and only for responseType "document": Aura has no XML parser for documents. */
        if (this._type === 'document' && (mime === 'text/html' || mime === '') && typeof DOMParser === 'function') {
            try { this._doc = new DOMParser().parseFromString(this._text(), 'text/html'); } catch (e) { this._doc = null; }
        }
        return this._doc;
    }
    get responseXML() {
        if (this._type !== '' && this._type !== 'document') throw domErr('responseXML needs responseType "" or "document"', 'InvalidStateError');
        return this._document();
    }
    get response() {
        if (this._type === '' || this._type === 'text') return this.responseText;
        if (this._state !== 4 || this._error) return null;
        if (this._resp !== null) return this._resp;
        if (this._type === 'json') { try { this._resp = JSON.parse(N.utf8Decode(this._bytes, false, true)); } catch (e) { this._resp = null; } }
        else if (this._type === 'arraybuffer') this._resp = this._bytes.slice().buffer;
        else if (this._type === 'blob') { const b = new Blob([], { type: this._finalType() }); b._b = this._bytes; this._resp = b; }
        else if (this._type === 'document') this._resp = this._document();
        return this._resp;
    }
}
for (const [k, v] of [['UNSENT', 0], ['OPENED', 1], ['HEADERS_RECEIVED', 2], ['LOADING', 3], ['DONE', 4]]) {
    XMLHttpRequest[k] = v;
    XMLHttpRequest.prototype[k] = v;
}
defineHandlers(XMLHttpRequest.prototype, ['readystatechange']);
for (const C of [XMLHttpRequestEventTarget, XMLHttpRequestUpload, XMLHttpRequest]) { tag(C); defGlobal(C.name, C); }

/* ---------------- EventSource ---------------- */

/* EventSource lives in src/js/eventsource.js, compiled to its own
 * bytecode and read with N.eventSource() the first time script touches
 * the global. */
let esLib = null;
const eventSources = () => esLib || (esLib = N.eventSource()(G, { N, domErr, EventTarget, defineHandlers, startFetch,
    fire, fireSimple, MessageEvent, CloseEvent, Blob, traceNet: P.traceNet }));
for (const name of ['EventSource', 'WebSocket']) {
    Object.defineProperty(G, name, {
        get() { const v = eventSources()[name]; defGlobal(name, v); return v; },
        set(v) { defGlobal(name, v); },
        configurable: true, enumerable: false,
    });
}
/* IndexedDB lives in src/js/idb.js, read with N.lazy(3) the first time
 * script touches one of these globals. */
lazyModule(3, ['indexedDB', 'IDBFactory', 'IDBRequest', 'IDBOpenDBRequest', 'IDBDatabase', 'IDBTransaction',
    'IDBObjectStore', 'IDBIndex', 'IDBCursor', 'IDBCursorWithValue', 'IDBKeyRange', 'IDBVersionChangeEvent'],
    () => ({ N, domErr, Event, EventTarget, defineHandlers, fire, fireSimple, structuredClone, eventParents, Blob }));
/* URLPattern lives in src/js/urlpattern.js, read with N.lazy(4). */
lazyModule(4, ['URLPattern'], () => ({}));

/* The document is hidden or shown again (P.pageTransition). */
P.eventSourcesFreeze = function (frozen) { if (esLib) esLib.freeze(frozen); };

/* Script loads for the page (external classic scripts loaded by src/page
 * go through the embedder's loader directly; this is for dynamically
 * inserted scripts and modules). dest 2 (a module) is fetched with CORS,
 * a classic script without. cb(text or null, message, finalUrl,
 * crossOrigin): crossOrigin is true when the script came from another
 * origin without CORS, and its errors are then muted. */
P.fetchText = function (url, dest, cb, cors) {
    const chunks = [];
    let status = 0, final = url, type = 'basic';
    try {
        startFetch('GET', url, [], null, cors === 2 ? 2 : 1, dest === 2 || cors ? 0 : 1, dest, {
            head(s, t, h, u, ty) { status = s; if (u) final = u; type = ty; },
            data(b) { chunks.push(b); },
            done(ok, msg) {
                cb(ok && (status === 0 || (status >= 200 && status < 300)) ? N.utf8Decode(concatBytes(chunks), false, false) : null,
                    msg, final, type === 'opaque');
            },
        });
    } catch (e) {
        N.queueTask(() => cb(null, e.message, url, false));
    }
};
