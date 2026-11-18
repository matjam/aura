/* Aura's EventSource (HTML, "Server-sent events") and WebSocket (at the
 * end of the file; they share the loading and the freezing). The build compiles
 * this file to its own bytecode (eventsource.bc); the prelude reads it
 * with N.eventSource() the first time script touches EventSource, so a
 * page that never does pays only for the getter. It is a function of G,
 * the global object, and H, the prelude's helpers, and returns the class
 * and freeze(), which the prelude calls when the document is hidden and
 * shown again.
 *
 * The request is an ordinary script request (CORS mode, credentials with
 * withCredentials, cache mode no-store, Accept: text/event-stream); its
 * body is parsed as it arrives by the C parser (N.sseFeed, src/net/sse.c).
 * A connection that ends or drops is made again after the reconnection
 * time, with Last-Event-ID; a response that is not a 200
 * text/event-stream, or a request the same-origin policy refuses, closes
 * the source for good.
 *
 * A document or worker keeps at most ES_MAX connections open at once
 * (each holds one of the loader's connections to its host for as long as
 * it lives, and on Mac OS 9 an Open Transport endpoint); more sources
 * wait, in the order they were made, in CONNECTING. A hidden document
 * (kept for Back and Forward) drops its connections and makes them again
 * when it is shown; a document that goes ends them with its requests.
 * docs/script-requests-and-cors.md ("EventSource") has the details. */
(function (G, H) {
'use strict';

const N = H.N;
const ES_MAX = 4;
const ES_RETRY = 3000;
const live = new Set();  /* sources with a request or a connection */
const waiting = [];      /* sources waiting for one of the ES_MAX slots */
let frozen = false;
const policyRefusal = /Access-Control|Cross-Origin|same-origin mode|file: and network|not http or https|credentials|unsupported URL scheme/;
const S = new WeakMap(); /* source -> its state */
const state = (o) => {
    const s = o !== null && typeof o === 'object' ? S.get(o) : undefined;
    if (!s) throw new TypeError('Illegal invocation');
    return s;
};

class EventSource extends H.EventTarget {
    constructor(url, init = undefined) {
        if (!arguments.length) throw new TypeError('EventSource: 1 argument required');
        super();
        let u;
        try { u = new URL(String(url), N.docUrl(1)); } catch (e) { throw H.domErr('Invalid URL: ' + String(url), 'SyntaxError'); }
        const s = { obj: this, url: u.href, cred: !!(init && init.withCredentials), state: 0, lastId: '', retry: ES_RETRY,
            abort: null, parser: null, timer: 0, origin: '' };
        S.set(this, s);
        request(s);
    }
    get url() { return state(this).url; }
    get withCredentials() { return state(this).cred; }
    get readyState() { return state(this).state; }
    close() {
        const s = state(this);
        s.state = 2;
        disconnect(s);
        if (s.timer) { N.clearTimer(s.timer); s.timer = 0; }
        const i = waiting.indexOf(s);
        if (i >= 0) waiting.splice(i, 1);
    }
}
for (const [k, v] of [['CONNECTING', 0], ['OPEN', 1], ['CLOSED', 2]]) {
    Object.defineProperty(EventSource, k, { value: v, enumerable: true });
    Object.defineProperty(EventSource.prototype, k, { value: v, enumerable: true });
}
H.defineHandlers(EventSource.prototype, ['open', 'message', 'error']);
Object.defineProperty(EventSource.prototype, Symbol.toStringTag, { value: 'EventSource', configurable: true });

/* Ends a source's request and parser (no events). */
function disconnect(s) {
    if (s.abort) { const a = s.abort; s.abort = null; a(); }
    if (s.parser) { N.sseEnd(s.parser); s.parser = null; }
    if (live.delete(s)) next();
}
/* A slot came free: the oldest waiting source connects. */
function next() {
    while (waiting.length && live.size < ES_MAX && !frozen) {
        const s = waiting.shift();
        if (s.state === 0) request(s);
    }
}
/* "Fail the connection": closed for good. */
function fail(s) {
    if (s.state === 2) return;
    s.state = 2;
    disconnect(s);
    H.fireSimple(s.obj, 'error');
}
/* "Reestablish the connection": error now, a new request after the
 * reconnection time. */
function reconnect(s) {
    disconnect(s);
    if (s.state === 2) return;
    s.state = 0;
    H.fireSimple(s.obj, 'error');
    if (s.state !== 0 || s.timer) return;
    s.timer = N.setTimeout(() => {
        s.timer = 0;
        if (s.state === 0) request(s);
    }, s.retry);
}
function request(s) {
    if (frozen || live.size >= ES_MAX) {
        if (!waiting.includes(s)) waiting.push(s);
        return;
    }
    live.add(s);
    const headers = [['Accept', 'text/event-stream']];
    if (s.lastId !== '') headers.push(['Last-Event-ID', s.lastId]);
    let opened = false;
    try {
        s.abort = H.startFetch('GET', s.url.replace(/#.*$/, ''), headers, null, s.cred ? 2 : 1, 0, 0, {
            head(status, text, hdrs, finalUrl, type) {
                if (s.state === 2) return;
                const ct = hdrs.find((p) => p[0].toLowerCase() === 'content-type');
                const mime = ct ? ct[1].split(';')[0].trim().toLowerCase() : '';
                if (status !== 200 || mime !== 'text/event-stream' || type === 'opaque' || type === 'opaqueredirect') {
                    fail(s);
                    return;
                }
                try { s.origin = new URL(finalUrl || s.url).origin; } catch (e) { s.origin = ''; }
                try { s.parser = N.sseNew(s.lastId); } catch (e) { fail(s); return; }
                opened = true;
                s.state = 1;
                H.fireSimple(s.obj, 'open');
            },
            data(bytes) {
                if (!s.parser || s.state === 2) return;
                for (const r of N.sseFeed(s.parser, bytes)) {
                    if (s.state === 2) return;
                    if (r[0] === 'r') s.retry = r[1];
                    else if (r[0] === 'i') s.lastId = r[1];
                    else {
                        s.lastId = r[3];
                        H.fire(s.obj, new H.MessageEvent(r[1], { data: r[2], origin: s.origin, lastEventId: r[3] }));
                    }
                }
            },
            done(ok, msg) {
                s.abort = null;
                if (s.state === 2) return;
                /* a refusal by the same-origin policy will not change */
                if (!ok && !opened && policyRefusal.test(msg || '')) fail(s);
                else reconnect(s);
            },
        }, { cache: 1 /* no-store */ });
    } catch (e) {
        /* refused before it went out: the failure is a task of its own */
        s.abort = null;
        live.delete(s);
        N.queueTask(() => fail(s));
    }
}
/* ---------------- WebSocket ----------------
 *
 * The opening handshake is a script request in WebSocket mode
 * (src/js/fetch.c: Origin, cookies, no CORS, no redirects) to the http
 * or https form of the URL; src/net/http.c checks the 101 response and
 * its Sec-WebSocket-Accept, and from then on the request's body is the
 * connection's bytes, which N.wsFeed parses (src/net/websocket.c), and
 * N.fetchSend writes the masked frames N.wsFrame builds. A ws: URL from
 * a secure document is refused. docs/script-requests-and-cors.md
 * ("WebSocket") has the details. */
const WS_MAX_MESSAGE = 4 * 1024 * 1024;
const WS_CLOSE_WAIT = 5000; /* after our close frame, how long the server has to close */
const wsLive = new Set();
const W = new WeakMap();
const wsState = (o) => {
    const s = o !== null && typeof o === 'object' ? W.get(o) : undefined;
    if (!s) throw new TypeError('Illegal invocation');
    return s;
};
const tokenRe = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const utf8Len = (str) => {
    let n = 0;
    for (const ch of str) {
        const c = ch.codePointAt(0);
        n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    }
    return n;
};

class WebSocket extends H.EventTarget {
    constructor(url, protocols = undefined) {
        if (!arguments.length) throw new TypeError('WebSocket: 1 argument required');
        super();
        let u;
        try { u = new URL(String(url), N.docUrl(1)); } catch (e) { throw H.domErr('Invalid URL: ' + String(url), 'SyntaxError'); }
        if (u.protocol === 'http:') u.protocol = 'ws:';
        else if (u.protocol === 'https:') u.protocol = 'wss:';
        if (u.protocol !== 'ws:' && u.protocol !== 'wss:')
            throw H.domErr("The URL's scheme must be 'ws' or 'wss'", 'SyntaxError');
        if (u.hash || String(url).includes('#'))
            throw H.domErr('The URL contains a fragment identifier', 'SyntaxError');
        let list = protocols === undefined ? [] : typeof protocols === 'string' ? [protocols] : Array.from(protocols, String);
        const seen = new Set();
        for (const p of list) {
            if (!tokenRe.test(p) || seen.has(p.toLowerCase()))
                throw H.domErr("The subprotocol '" + p + "' is invalid", 'SyntaxError');
            seen.add(p.toLowerCase());
        }
        let docProto = '';
        try { docProto = new URL(N.docUrl(1)).protocol; } catch (e) { /* about:blank */ }
        if (u.protocol === 'ws:' && docProto === 'https:')
            throw H.domErr('An insecure WebSocket connection may not be initiated from a page loaded over HTTPS', 'SecurityError');
        const s = { obj: this, url: u.href, state: 0, protocol: '', binaryType: 'blob', buffered: 0, abort: null,
            parser: null, sentClose: false, gotClose: false, code: 1006, reason: '', timer: 0, origin: u.origin,
            protocols: list, ended: false };
        W.set(this, s);
        wsLive.add(s);
        const httpUrl = (u.protocol === 'wss:' ? 'https:' : 'http:') + u.href.slice(u.protocol.length);
        const headers = list.length ? [['Sec-WebSocket-Protocol', list.join(', ')]] : [];
        try {
            s.abort = H.startFetch('GET', httpUrl, headers, null, 2, 4 /* WebSocket */, 0, {
                head(status, text, hdrs) { wsHead(s, status, hdrs); },
                data(bytes) { wsData(s, bytes); },
                done(ok) { s.abort = null; wsEnded(s); },
            }, { cache: 1 });
        } catch (e) {
            s.abort = null;
            N.queueTask(() => wsFail(s));
        }
    }
    get url() { return wsState(this).url; }
    get readyState() { return wsState(this).state; }
    get bufferedAmount() { return wsState(this).buffered; }
    get extensions() { wsState(this); return ''; }
    get protocol() { return wsState(this).protocol; }
    get binaryType() { return wsState(this).binaryType; }
    set binaryType(v) {
        const s = wsState(this);
        if (v === 'blob' || v === 'arraybuffer') s.binaryType = v;
    }
    send(data) {
        const s = wsState(this);
        if (arguments.length < 1) throw new TypeError('WebSocket.send: 1 argument required');
        if (s.state === 0) throw H.domErr('Still in CONNECTING state', 'InvalidStateError');
        let op = 2, payload;
        if (data instanceof ArrayBuffer) payload = new Uint8Array(data);
        else if (ArrayBuffer.isView(data)) payload = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        else if (H.Blob && data instanceof H.Blob) payload = data._b;
        else { op = 1; payload = String(data); }
        const len = op === 1 ? utf8Len(payload) : payload.length;
        s.buffered += len;
        if (s.state !== 1) return;
        if (!wsWrite(s, op, payload)) { wsFail(s, 1006); return; }
        /* the bytes are on their way: the amount drops once script yields */
        N.queueTask(() => { s.buffered = Math.max(0, s.buffered - len); });
    }
    close(code = undefined, reason = undefined) {
        const s = wsState(this);
        if (code !== undefined) {
            code = Number(code) & 0xffff;
            if (code !== 1000 && !(code >= 3000 && code <= 4999))
                throw H.domErr('The code must be 1000 or between 3000 and 4999', 'InvalidAccessError');
        }
        reason = reason === undefined ? '' : String(reason);
        if (utf8Len(reason) > 123) throw H.domErr('The close reason is longer than 123 bytes', 'SyntaxError');
        if (s.state >= 2) return;
        if (s.state === 0) {
            /* fail the connection: error, then close */
            wsFail(s, 1006);
            return;
        }
        s.state = 2;
        wsSendClose(s, code, reason);
    }
}
for (const [k, v] of [['CONNECTING', 0], ['OPEN', 1], ['CLOSING', 2], ['CLOSED', 3]]) {
    Object.defineProperty(WebSocket, k, { value: v, enumerable: true });
    Object.defineProperty(WebSocket.prototype, k, { value: v, enumerable: true });
}
H.defineHandlers(WebSocket.prototype, ['open', 'message', 'error', 'close']);
Object.defineProperty(WebSocket.prototype, Symbol.toStringTag, { value: 'WebSocket', configurable: true });

function wsWrite(s, op, payload) {
    if (!s.abort || !s.abort.send) return false;
    return s.abort.send(N.wsFrame(op, payload));
}
function wsSendClose(s, code, reason) {
    if (s.sentClose) return;
    s.sentClose = true;
    let body = new Uint8Array(0);
    if (code !== undefined) {
        const r = new TextEncoder().encode(reason || '');
        body = new Uint8Array(2 + r.length);
        body[0] = code >> 8;
        body[1] = code & 255;
        body.set(r, 2);
    }
    wsWrite(s, 8, body);
    /* the server closes the TCP connection; if it does not, Aura does */
    if (!s.timer) s.timer = N.setTimeout(() => { s.timer = 0; wsDrop(s); }, WS_CLOSE_WAIT);
}
/* Ends the connection without more events of its own (wsEnded follows
 * from done(), or is called here when the request already ended). */
function wsDrop(s) {
    if (s.abort) { const a = s.abort; s.abort = null; a(); }
    wsEnded(s);
}
function wsHead(s, status, hdrs) {
    if (s.state !== 0) return;
    const get = (n) => { const h = hdrs.find((p) => p[0].toLowerCase() === n); return h ? h[1] : null; };
    const proto = get('sec-websocket-protocol');
    if (status !== 101 || (proto !== null && !s.protocols.includes(proto))) {
        wsFail(s, 1006);
        return;
    }
    try { s.parser = N.wsNew(WS_MAX_MESSAGE); } catch (e) { wsFail(s, 1006); return; }
    s.protocol = proto || '';
    s.state = 1;
    H.fireSimple(s.obj, 'open');
}
function wsData(s, bytes) {
    if (!s.parser || s.state === 3) return;
    for (const r of N.wsFeed(s.parser, bytes)) {
        if (s.state === 3) return;
        switch (r[0]) {
        case 't':
        case 'b': {
            if (s.state !== 1) break; /* data after our close frame is dropped */
            let data = r[1];
            if (r[0] === 'b' && s.binaryType === 'blob') data = new H.Blob([data]);
            H.fire(s.obj, new H.MessageEvent('message', { data, origin: s.origin }));
            break;
        }
        case 'p':
            if (!s.sentClose) wsWrite(s, 10, r[1]);
            break;
        case 'o':
            break;
        case 'c':
            s.gotClose = true;
            s.code = r[1];
            s.reason = r[2];
            if (s.state === 1) s.state = 2;
            /* echo the code; the server then closes the connection */
            wsSendClose(s, r[1] === 1005 ? undefined : r[1], '');
            break;
        case 'e':
            wsFail(s, r[1]);
            return;
        }
    }
}
/* "Fail the WebSocket connection": a close frame when open, then error
 * and close with code 1006. */
function wsFail(s, sendCode) {
    if (s.state === 3) return;
    if (s.state === 1 && sendCode && sendCode !== 1006) wsSendClose(s, sendCode, '');
    s.gotClose = false;
    s.code = 1006;
    s.reason = '';
    s.failed = true;
    wsDrop(s);
}
/* The connection is gone: CLOSED, and the events. */
function wsEnded(s) {
    if (s.ended) return;
    s.ended = true;
    if (s.abort) { const a = s.abort; s.abort = null; a(); }
    if (s.timer) { N.clearTimer(s.timer); s.timer = 0; }
    if (s.parser) { N.wsEnd(s.parser); s.parser = null; }
    wsLive.delete(s);
    const clean = s.gotClose && s.sentClose && !s.failed;
    if (!clean && !s.gotClose) { s.code = 1006; s.reason = ''; }
    s.state = 3;
    const fireAll = () => {
        if (s.failed || !clean) H.fireSimple(s.obj, 'error');
        H.fire(s.obj, new H.CloseEvent('close', { wasClean: clean, code: s.code, reason: s.reason }));
    };
    /* a task of its own: close() from script fires nothing inside the call */
    if (frozen) wsHeld.push(fireAll); else N.queueTask(fireAll);
}
const wsHeld = []; /* close events of sockets a hidden document dropped */

/* The document is hidden (true) or shown again (false). */
function freeze(on) {
    frozen = on;
    if (on) {
        for (const s of [...live]) {
            disconnect(s);
            if (s.state !== 2) { s.state = 0; waiting.unshift(s); }
        }
        /* a hidden document keeps no WebSocket open: each closes (1001,
         * going away) and its close event comes when it is shown */
        for (const s of [...wsLive]) {
            if (s.state === 1) wsSendClose(s, 1001, '');
            s.failed = true;
            wsDrop(s);
        }
        return;
    }
    next();
    for (const f of wsHeld.splice(0)) N.queueTask(f);
}

return { EventSource, WebSocket, freeze };
})
