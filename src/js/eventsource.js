/* Aura's EventSource (HTML, "Server-sent events"). The build compiles
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
/* The document is hidden (true) or shown again (false). */
function freeze(on) {
    frozen = on;
    if (on) {
        for (const s of [...live]) {
            disconnect(s);
            if (s.state !== 2) { s.state = 0; waiting.unshift(s); }
        }
        return;
    }
    next();
}

return { EventSource, freeze };
})
