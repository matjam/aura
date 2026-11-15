
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
    stream() { throw new TypeError('Blob streams are not supported'); }
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
const forbiddenRequest = new Set(['accept-charset', 'accept-encoding', 'access-control-request-headers',
    'access-control-request-method', 'connection', 'content-length', 'cookie', 'cookie2', 'date', 'dnt', 'expect', 'host',
    'keep-alive', 'origin', 'referer', 'set-cookie', 'te', 'trailer', 'transfer-encoding', 'upgrade', 'via']);
const isForbiddenRequest = (n) => forbiddenRequest.has(n) || n.startsWith('proxy-') || n.startsWith('sec-');
class Headers {
    constructor(init) {
        this._l = [];
        this._guard = 'none';
        if (init === undefined || init === null) return;
        if (init instanceof Headers) { this._l = init._l.map((p) => p.slice()); return; }
        if (typeof init !== 'object') throw new TypeError('Headers: invalid init');
        if (typeof init[Symbol.iterator] === 'function') {
            for (const pair of init) {
                const a = Array.from(pair);
                if (a.length !== 2) throw new TypeError('Headers: each pair must have two items');
                this.append(a[0], a[1]);
            }
        } else for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    _check(n) {
        if (this._guard === 'immutable') throw new TypeError('The headers are immutable');
        return this._guard === 'request' && isForbiddenRequest(n);
    }
    append(n, v) {
        n = headerName(n); v = headerValue(v);
        if (this._check(n)) return;
        this._l.push([n, v]);
    }
    delete(n) { n = headerName(n); if (this._check(n)) return; this._l = this._l.filter((p) => p[0] !== n); }
    get(n) {
        n = headerName(n);
        const vs = this._l.filter((p) => p[0] === n).map((p) => p[1]);
        return vs.length ? vs.join(', ') : null;
    }
    getSetCookie() { return this._l.filter((p) => p[0] === 'set-cookie').map((p) => p[1]); }
    has(n) { n = headerName(n); return this._l.some((p) => p[0] === n); }
    set(n, v) {
        n = headerName(n); v = headerValue(v);
        if (this._check(n)) return;
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

/* Extracts a body: [bytes or null, content type or null]. */
function extractBody(body) {
    if (body === null || body === undefined) return [null, null];
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
const bodyMixin = {
    get body() { return null; },
    get bodyUsed() { return !!this._used; },
    _consume() {
        if (this._used) return Promise.reject(new TypeError('The body has already been read'));
        this._used = true;
        return this._bodyPromise ? this._bodyPromise() : Promise.resolve(this._bytes || new Uint8Array(0));
    },
    arrayBuffer() { return this._consume().then((b) => b.slice().buffer); },
    bytes() { return this._consume().then((b) => b.slice()); },
    text() { return this._consume().then((b) => N.utf8Decode(b, false, false)); },
    json() { return this.text().then((t) => JSON.parse(t)); },
    blob() { return this._consume().then((b) => { const x = new Blob([], { type: (this.headers.get('content-type') || '') }); x._b = b; return x; }); },
    formData() {
        return this.text().then((t) => {
            const ct = this.headers.get('content-type') || '';
            if (!/^application\/x-www-form-urlencoded/i.test(ct)) throw new TypeError('Only urlencoded form bodies can be parsed');
            const fd = new FormData();
            for (const [k, v] of new URLSearchParams(t)) fd.append(k, v);
            return fd;
        });
    },
};

const modeNum = { cors: 0, 'no-cors': 1, 'same-origin': 2, navigate: 3 };
const credNum = { omit: 0, 'same-origin': 1, include: 2 };
class Request {
    constructor(input, init = {}) {
        let src = null;
        if (input instanceof Request) src = input;
        const url = src ? src.url : new URL(str(input), N.docUrl(1)).href;
        if (/^[a-z][a-z0-9+.-]*:\/\/[^/]*@/i.test(url)) throw new TypeError('A request URL must not include credentials');
        init = init || {};
        let method = init.method !== undefined ? str(init.method) : src ? src.method : 'GET';
        if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(method)) throw new TypeError('Invalid method');
        const up = method.toUpperCase();
        if (/^(CONNECT|TRACE|TRACK)$/.test(up)) throw new TypeError('Forbidden method: ' + method);
        if (/^(DELETE|GET|HEAD|OPTIONS|POST|PUT|PATCH)$/.test(up)) method = up;
        this._url = url;
        this._method = method;
        this._mode = init.mode !== undefined ? str(init.mode) : src ? src.mode : 'cors';
        if (!(this._mode in modeNum) || this._mode === 'navigate') throw new TypeError('Invalid mode');
        this._cred = init.credentials !== undefined ? str(init.credentials) : src ? src.credentials : 'same-origin';
        if (!(this._cred in credNum)) throw new TypeError('Invalid credentials');
        this._cache = init.cache || (src ? src.cache : 'default');
        this._redirect = init.redirect || (src ? src.redirect : 'follow');
        this._signal = init.signal || (src ? src.signal : null) || new AbortController().signal;
        this.headers = new Headers(init.headers !== undefined ? init.headers : src ? src.headers : undefined);
        this.headers._guard = this._mode === 'no-cors' ? 'request-no-cors' : 'request';
        for (const [k] of this.headers._l.slice()) if (isForbiddenRequest(k)) this.headers._l = this.headers._l.filter((p) => p[0] !== k);
        const body = init.body !== undefined ? init.body : null;
        if (body !== null && (method === 'GET' || method === 'HEAD')) throw new TypeError('A GET or HEAD request cannot have a body');
        if (body !== null) {
            const [bytes, type] = extractBody(body);
            this._bytes = bytes;
            if (type && !this.headers.has('content-type')) this.headers._l.push(['content-type', type]);
        } else if (src) {
            if (src._used) throw new TypeError('The source request body has been used');
            this._bytes = src._bytes;
        } else this._bytes = null;
    }
    get url() { return this._url; }
    get method() { return this._method; }
    get mode() { return this._mode; }
    get credentials() { return this._cred; }
    get cache() { return this._cache; }
    get redirect() { return this._redirect; }
    get signal() { return this._signal; }
    get destination() { return ''; }
    get referrer() { return 'about:client'; }
    get referrerPolicy() { return ''; }
    get integrity() { return ''; }
    get keepalive() { return false; }
    clone() { if (this._used) throw new TypeError('The body has been used'); return new Request(this); }
}
defProps(Request.prototype, bodyMixin);
tag(Request);
defGlobal('Request', Request);

const redirectStatus = new Set([301, 302, 303, 307, 308]);
class Response {
    constructor(body = null, init = {}) {
        init = init || {};
        const status = init.status !== undefined ? Number(init.status) : 200;
        if (!(status >= 200 && status <= 599) || status !== (status | 0)) throw new RangeError('Invalid status');
        this._status = status;
        this._statusText = init.statusText !== undefined ? str(init.statusText) : '';
        this.headers = new Headers(init.headers);
        this._type = 'default';
        this._url = '';
        this._redirected = false;
        if (body !== null) {
            if (status === 101 || status === 204 || status === 205 || status === 304) throw new TypeError('This status cannot have a body');
            const [bytes, type] = extractBody(body);
            this._bytes = bytes;
            if (type && !this.headers.has('content-type')) this.headers.append('content-type', type);
        } else this._bytes = null;
    }
    static error() { const r = new Response(); r._type = 'error'; r._status = 0; r.headers._guard = 'immutable'; return r; }
    static redirect(url, status = 302) {
        if (!redirectStatus.has(status)) throw new RangeError('Invalid redirect status');
        const r = new Response(null, { status });
        r.headers.set('location', new URL(str(url), N.docUrl(1)).href);
        r.headers._guard = 'immutable';
        return r;
    }
    static json(data, init = {}) {
        const s = JSON.stringify(data);
        if (s === undefined) throw new TypeError('The value cannot be serialized as JSON');
        const r = new Response(s, init);
        r.headers.set('content-type', 'application/json');
        return r;
    }
    get type() { return this._type; }
    get url() { return this._url; }
    get redirected() { return this._redirected; }
    get status() { return this._status; }
    get ok() { return this._status >= 200 && this._status <= 299; }
    get statusText() { return this._statusText; }
    clone() {
        if (this._used) throw new TypeError('The body has been used');
        const r = Object.create(Response.prototype);
        Object.assign(r, this);
        r.headers = new Headers(this.headers);
        r.headers._guard = this.headers._guard;
        if (this._bodyPromise) {
            const p = this._bodyPromise();
            this._bodyPromise = r._bodyPromise = () => p;
        }
        return r;
    }
}
defProps(Response.prototype, bodyMixin);
tag(Response);
defGlobal('Response', Response);

/* Starts a request through the embedder. Calls onHead(status, text,
 * headers, finalUrl, opaque), onData(bytes), onDone(ok, message). Returns
 * an abort function. data: and blob: URLs are answered here. */
function startFetch(method, url, headerList, bytes, cred, mode, dest, cb) {
    if (/^data:/i.test(url)) {
        const m = /^data:([^,]*?)(;base64)?,(.*)$/is.exec(url);
        N.queueTask(() => {
            if (!m) { cb.done(false, 'invalid data: URL'); return; }
            let body;
            try {
                if (m[2]) { const bin = atob(decodeURIComponent(m[3]).replace(/\s/g, '')); body = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) body[i] = bin.charCodeAt(i); }
                else body = N.utf8Encode(decodeURIComponent(m[3]));
            } catch (e) { cb.done(false, 'invalid data: URL'); return; }
            cb.head(200, 'OK', [['content-type', m[1] || 'text/plain;charset=US-ASCII']], url, false);
            cb.data(body);
            cb.done(true);
        });
        return () => {};
    }
    if (/^blob:/i.test(url)) {
        const blob = blobUrls.get(url);
        N.queueTask(() => {
            if (!blob || method !== 'GET') { cb.done(false, 'the blob URL is not valid'); return; }
            cb.head(200, 'OK', [['content-type', blob.type], ['content-length', String(blob.size)]], url, false);
            cb.data(blob._b);
            cb.done(true);
        });
        return () => {};
    }
    let id = 0;
    let finished = false;
    id = N.fetchStart(method, url, headerList, bytes, cred, mode, dest, (msg) => {
        if (finished) return;
        switch (msg[0]) {
        case 'head': cb.head(msg[1], msg[2], msg[3], msg[4] || url, msg[5]); break;
        case 'data': cb.data(msg[1]); break;
        case 'done': finished = true; cb.done(true); break;
        case 'error': finished = true; cb.done(false, msg[1] || 'network error'); break;
        }
    });
    return () => { if (!finished) { finished = true; N.fetchAbort(id); } };
}

defGlobal('fetch', function fetch(input, init) {
    let req;
    try {
        req = new Request(input, init);
    } catch (e) {
        return Promise.reject(e);
    }
    return new Promise((resolve, reject) => {
        const signal = req.signal;
        if (signal && signal.aborted) { reject(signal.reason); return; }
        const chunks = [];
        let response = null;
        let bodyResolve, bodyReject;
        const bodyP = new Promise((a, b) => { bodyResolve = a; bodyReject = b; });
        bodyP.catch(() => {});
        let abort;
        const onAbort = () => {
            abort();
            const reason = signal.reason;
            if (!response) reject(reason); else bodyReject(reason);
        };
        try {
            abort = startFetch(req.method, req.url, req.headers._l, req._bytes, credNum[req.credentials], modeNum[req.mode], 0, {
                head(status, text, headers, finalUrl, opaque) {
                    const r = Object.create(Response.prototype);
                    r._status = status;
                    r._statusText = text;
                    r.headers = new Headers();
                    for (const [k, v] of headers) { try { r.headers.append(k, v); } catch (e) { /* malformed header: dropped */ } }
                    r.headers._guard = 'immutable';
                    r._type = opaque ? 'opaque' : N.sameOrigin(finalUrl) ? 'basic' : 'cors';
                    r._url = opaque ? '' : finalUrl;
                    r._redirected = finalUrl !== req.url;
                    r._bodyPromise = () => bodyP;
                    response = r;
                    resolve(r);
                },
                data(b) { chunks.push(b); },
                done(ok, msg) {
                    if (signal) signal.removeEventListener('abort', onAbort);
                    if (ok) {
                        if (!response) this.head(200, 'OK', [], req.url, false);
                        bodyResolve(concatBytes(chunks));
                    } else {
                        const e = new TypeError('Failed to fetch: ' + (msg || 'network error'));
                        if (!response) reject(e); else bodyReject(e);
                    }
                },
            });
        } catch (e) {
            reject(e);
            return;
        }
        if (signal) signal.addEventListener('abort', onAbort);
    });
});

/* ---------------- XMLHttpRequest ---------------- */

class XMLHttpRequestEventTarget extends EventTarget {}
defineHandlers(XMLHttpRequestEventTarget.prototype, ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend']);
class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}
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
        this._abort = null;
        this._timeout = 0;
        this._timer = 0;
        this._cred = false;
        this._upload = new XMLHttpRequestUpload();
        this._url = '';
        this._send = false;
        this._error = false;
        this._mime = null;
    }
    get readyState() { return this._state; }
    get status() { return this._error ? 0 : this._status; }
    get statusText() { return this._error ? '' : this._statusText; }
    get responseURL() { return this._error ? '' : this._finalUrl || ''; }
    get upload() { return this._upload; }
    get timeout() { return this._timeout; }
    set timeout(v) { this._timeout = v >>> 0; }
    get withCredentials() { return this._cred; }
    set withCredentials(v) {
        if (this._state > 1 || this._send) throw domErr('The request has started', 'InvalidStateError');
        this._cred = !!v;
    }
    get responseType() { return this._type; }
    set responseType(v) {
        if (this._state === 3 || this._state === 4) throw domErr('The response is loading or done', 'InvalidStateError');
        if (['', 'text', 'json', 'arraybuffer', 'blob', 'document'].includes(v)) this._type = v;
    }
    _setState(s) {
        this._state = s;
        fireSimple(this, 'readystatechange', false, false);
    }
    open(method, url, async = true, user, password) {
        method = str(method);
        if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(method)) throw domErr('Invalid method', 'SyntaxError');
        const up = method.toUpperCase();
        if (/^(CONNECT|TRACE|TRACK)$/.test(up)) throw domErr('Forbidden method', 'SecurityError');
        if (/^(DELETE|GET|HEAD|OPTIONS|POST|PUT|PATCH)$/.test(up)) method = up;
        let u;
        try { u = new URL(str(url), N.docUrl(1)); } catch (e) { throw domErr('Invalid URL', 'SyntaxError'); }
        if (async === false) throw domErr('Synchronous XMLHttpRequest is not supported', 'InvalidAccessError');
        if (this._abort) { this._abort(); this._abort = null; }
        this._method = method;
        this._url = u.href;
        this._headers = [];
        this._send = false;
        this._resp = null;
        this._chunks = [];
        this._status = 0;
        this._statusText = '';
        this._respHeaders = [];
        this._error = false;
        this._setState(1);
    }
    setRequestHeader(n, v) {
        if (this._state !== 1 || this._send) throw domErr('The request is not opened', 'InvalidStateError');
        n = headerName(n); v = headerValue(v);
        if (isForbiddenRequest(n)) return;
        const h = this._headers.find((p) => p[0] === n);
        if (h) h[1] += ', ' + v; else this._headers.push([n, v]);
    }
    overrideMimeType(m) { this._mime = str(m); }
    send(body = null) {
        if (this._state !== 1 || this._send) throw domErr('The request is not opened', 'InvalidStateError');
        if (this._method === 'GET' || this._method === 'HEAD') body = null;
        let bytes = null;
        if (body !== null) {
            const [b, type] = extractBody(body);
            bytes = b;
            if (type && !this._headers.some((p) => p[0] === 'content-type')) this._headers.push(['content-type', type]);
        }
        this._send = true;
        const xhr = this;
        fire(this, new ProgressEvent('loadstart', { total: 0 }));
        if (this._state !== 1 || !this._send) return;
        let loaded = 0;
        try {
            this._abort = startFetch(this._method, this._url, this._headers, bytes,
                this._cred ? 2 : 1, 0, 0, {
                    head(status, text, headers, finalUrl, opaque) {
                        xhr._status = status; xhr._statusText = text; xhr._respHeaders = headers; xhr._finalUrl = finalUrl;
                        xhr._setState(2);
                    },
                    data(b) {
                        xhr._chunks.push(b);
                        loaded += b.length;
                        if (xhr._state === 2) xhr._setState(3);
                        else fireSimple(xhr, 'readystatechange', false, false);
                        fire(xhr, new ProgressEvent('progress', { loaded, total: 0 }));
                    },
                    done(ok, msg) {
                        if (xhr._timer) { G.clearTimeout(xhr._timer); xhr._timer = 0; }
                        xhr._abort = null;
                        if (!ok) { xhr._fail('error'); return; }
                        if (xhr._state < 2) xhr._setState(2);
                        if (xhr._state < 3) xhr._setState(3);
                        xhr._send = false;
                        xhr._bytes = concatBytes(xhr._chunks);
                        xhr._chunks = [];
                        xhr._setState(4);
                        fire(xhr, new ProgressEvent('load', { lengthComputable: true, loaded, total: loaded }));
                        fire(xhr, new ProgressEvent('loadend', { lengthComputable: true, loaded, total: loaded }));
                    },
                });
        } catch (e) {
            N.queueTask(() => this._fail('error'));
            return;
        }
        if (this._timeout) this._timer = G.setTimeout(() => { if (this._abort) { this._abort(); this._abort = null; } this._fail('timeout'); }, this._timeout);
    }
    _fail(kind) {
        this._error = true;
        this._send = false;
        this._bytes = new Uint8Array(0);
        this._status = 0;
        this._respHeaders = [];
        this._setState(4);
        fire(this, new ProgressEvent(kind));
        fire(this, new ProgressEvent('loadend'));
    }
    abort() {
        if (this._abort) { this._abort(); this._abort = null; }
        if ((this._state === 1 && this._send) || this._state === 2 || this._state === 3) {
            this._error = true;
            this._send = false;
            this._setState(4);
            fire(this, new ProgressEvent('abort'));
            fire(this, new ProgressEvent('loadend'));
        }
        if (this._state === 4) this._state = 0;
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
        for (const [k, v] of h) if (k !== 'set-cookie') s += k + ': ' + v + '\r\n';
        return s;
    }
    _text() {
        const b = this._bytes || concatBytes(this._chunks);
        return N.utf8Decode(b, false, false);
    }
    get responseText() {
        if (this._type !== '' && this._type !== 'text') throw domErr('responseText needs responseType "" or "text"', 'InvalidStateError');
        if (this._state < 3) return '';
        return this._text();
    }
    get responseXML() { return null; }
    get response() {
        if (this._type === '' || this._type === 'text') return this.responseText;
        if (this._state !== 4 || this._error) return null;
        if (this._resp !== null) return this._resp;
        if (this._type === 'json') { try { this._resp = JSON.parse(this._text()); } catch (e) { this._resp = null; } }
        else if (this._type === 'arraybuffer') this._resp = this._bytes.slice().buffer;
        else if (this._type === 'blob') { const b = new Blob([], { type: this._mime || this.getResponseHeader('content-type') || '' }); b._b = this._bytes; this._resp = b; }
        return this._resp;
    }
}
for (const [k, v] of [['UNSENT', 0], ['OPENED', 1], ['HEADERS_RECEIVED', 2], ['LOADING', 3], ['DONE', 4]]) {
    XMLHttpRequest[k] = v;
    XMLHttpRequest.prototype[k] = v;
}
defineHandlers(XMLHttpRequest.prototype, ['readystatechange']);
for (const C of [XMLHttpRequestEventTarget, XMLHttpRequestUpload, XMLHttpRequest]) { tag(C); defGlobal(C.name, C); }

/* Script loads for the page (external classic scripts loaded by src/page
 * go through the embedder's loader directly; this is for dynamically
 * inserted scripts and modules). */
P.fetchText = function (url, dest, cb) {
    const chunks = [];
    let status = 0;
    try {
        startFetch('GET', url, [], null, 1, dest === 2 ? 0 : 1, dest, {
            head(s) { status = s; },
            data(b) { chunks.push(b); },
            done(ok, msg) { cb(ok && (status === 0 || (status >= 200 && status < 300)) ? N.utf8Decode(concatBytes(chunks), false, false) : null, msg); },
        });
    } catch (e) {
        N.queueTask(() => cb(null, e.message));
    }
};
