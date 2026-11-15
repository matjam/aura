
/* ---------------- window ---------------- */

const WP = Window.prototype;
const settings = N.settings();
defProps(WP, {
    get window() { return G; },
    get self() { return G; },
    set self(v) { defGlobal('self', v); },
    get frames() { return G; },
    get parent() { return G; },
    get top() { return G; },
    get opener() { return null; },
    set opener(v) {},
    get frameElement() { return null; },
    get length() { return 0; },
    get closed() { return false; },
    get name() { return ''; },
    set name(v) {},
    get status() { return ''; },
    set status(v) {},
    get document() { return doc; },
    get origin() { return N.docUrl(2); },
    get isSecureContext() { return /^(https:|file:)/.test(N.docUrl(0)) || /^http:\/\/(localhost|127\.0\.0\.1)[:/]/.test(N.docUrl(0)); },
    get crossOriginIsolated() { return false; },
    get event() { return currentEvent(); },
    get innerWidth() { return N.viewport()[0]; },
    get innerHeight() { return N.viewport()[1]; },
    get outerWidth() { return N.viewport()[0]; },
    get outerHeight() { return N.viewport()[1]; },
    get scrollX() { return N.viewport()[2]; },
    get scrollY() { return N.viewport()[3]; },
    get pageXOffset() { return N.viewport()[2]; },
    get pageYOffset() { return N.viewport()[3]; },
    get screenX() { return 0; },
    get screenY() { return 0; },
    get screenLeft() { return 0; },
    get screenTop() { return 0; },
    get devicePixelRatio() { return 1; },
    scrollTo(x, y) {
        const v = N.viewport();
        if (x && typeof x === 'object') { y = 'top' in x ? x.top : v[3]; x = 'left' in x ? x.left : v[2]; }
        N.scrollTo(+x || 0, +y || 0);
    },
    scroll(x, y) { G.scrollTo(x, y); },
    scrollBy(x, y) {
        const v = N.viewport();
        if (x && typeof x === 'object') { y = x.top || 0; x = x.left || 0; }
        N.scrollTo(v[2] + (+x || 0), v[3] + (+y || 0));
    },
    alert(msg) { N.alert(arguments.length ? str(msg) : ''); },
    confirm(msg) { return N.confirm(arguments.length ? str(msg) : ''); },
    prompt(msg, def) { return N.prompt(msg === undefined ? '' : str(msg), def === undefined ? '' : str(def)); },
    print() {},
    stop() {},
    focus() {},
    blur() {},
    close() {},
    moveTo() {}, moveBy() {}, resizeTo() {}, resizeBy() {},
    open(url = '', target = '_blank', features = '') {
        url = str(url);
        const abs = url === '' ? 'about:blank' : resolveUrl(url);
        target = str(target) || '_blank';
        if (target === '_self' || target === '_top' || target === '_parent') { G.location.assign(abs); return G; }
        return N.openWindow(abs, target) ? null : null;
    },
    getComputedStyle: G.getComputedStyle,
    matchMedia(q) { return new MediaQueryList(str(q)); },
    postMessage(data, origin) {
        if (!arguments.length) throw new TypeError('postMessage: 1 argument required');
        let o = typeof origin === 'object' && origin ? (origin.targetOrigin === undefined ? '/' : str(origin.targetOrigin))
            : str(origin === undefined ? '/' : origin);
        /* targetOrigin is "*", "/" or any absolute URL, of which only the
         * origin counts (Google's search box passes location.href). */
        if (o !== '*' && o !== '/') {
            let u;
            try { u = new URL(o); } catch (e) { throw new DOMException("postMessage: invalid target origin '" + o + "'", 'SyntaxError'); }
            const mine = N.docUrl(2);
            if (u.origin !== mine && !(u.protocol === 'file:' && mine === 'file://')) return;
        }
        const cloned = structuredClone(data);
        N.queueTask(() => fire(G, new MessageEvent('message', { data: cloned, origin: N.docUrl(2), source: G })));
    },
    captureEvents() {}, releaseEvents() {},
    requestIdleCallback(cb, opts) {
        if (typeof cb !== 'function') throw new TypeError('requestIdleCallback: callback is not a function');
        return N.setTimeout(() => cb({ didTimeout: false, timeRemaining: () => 10 }), 1);
    },
    cancelIdleCallback(id) { N.clearTimer(id >>> 0); },
});
let currentEventValue;
function currentEvent() { return currentEventValue; }
P.setCurrentEvent = (e) => { const o = currentEventValue; currentEventValue = e; return o; };

/* timers */
const timerFn = (fn) => (typeof fn === 'function' ? fn : str(fn));
defGlobal('setTimeout', function setTimeout(fn, ms, ...args) { return N.setTimeout(timerFn(fn), ms, ...args); });
defGlobal('setInterval', function setInterval(fn, ms, ...args) { return N.setInterval(timerFn(fn), ms, ...args); });
defGlobal('clearTimeout', function clearTimeout(id) { if (id !== undefined) N.clearTimer(Number(id) >>> 0); });
defGlobal('clearInterval', function clearInterval(id) { if (id !== undefined) N.clearTimer(Number(id) >>> 0); });
defGlobal('requestAnimationFrame', function requestAnimationFrame(cb) {
    if (typeof cb !== 'function') throw new TypeError('requestAnimationFrame: callback is not a function');
    return N.requestAnimationFrame((t) => cb.call(G, t - timeOriginNow));
});
defGlobal('cancelAnimationFrame', function cancelAnimationFrame(id) { N.cancelAnimationFrame(Number(id) >>> 0); });
defGlobal('queueMicrotask', function queueMicrotask(cb) {
    if (typeof cb !== 'function') throw new TypeError('queueMicrotask: callback is not a function');
    Promise.resolve().then(() => cb());
});

/* ---------------- location ---------------- */

class Location {
    constructor() { throw new TypeError('Illegal constructor'); }
    get href() { return N.docUrl(0); }
    set href(v) { navigateTo(str(v), false); }
    assign(v) { navigateTo(str(v), false); }
    replace(v) { navigateTo(str(v), true); }
    reload() { N.reload(); }
    toString() { return N.docUrl(0); }
    get ancestorOrigins() { return makeList(NodeList, doc, -1, null, []); }
}
function navigateTo(url, replace) {
    const t = url.trim();
    if (/^javascript:/i.test(t)) { G.setTimeout(() => P.runJavascriptUrl(t), 0); return; }
    const abs = new URL(url, N.docUrl(1)).href;
    const cur = new URL(N.docUrl(0));
    const next = new URL(abs);
    if (next.hash && cur.href.replace(/#.*$/, '') === next.href.replace(/#.*$/, '')) {
        /* fragment navigation: no reload */
        const old = cur.href;
        N.historyPush(abs);
        if (old !== abs) N.queueTask(() => fire(G, new HashChangeEvent('hashchange', { oldURL: old, newURL: abs })));
        const id = decodeURIComponent(next.hash.slice(1));
        const el = id ? doc.getElementById(id) || doc.getElementsByName(id)[0] : null;
        if (el) el.scrollIntoView(); else if (!id || id.toLowerCase() === 'top') N.scrollTo(0, 0);
        return;
    }
    if (replace) N.navigateReplace(abs); else N.navigate(abs);
}
for (const k of urlPartNames) {
    Object.defineProperty(Location.prototype, k, {
        get() { return new URL(N.docUrl(0))[k]; },
        set(v) {
            if (k === 'origin') return;
            const u = new URL(N.docUrl(0));
            u[k] = v;
            navigateTo(u.href, false);
        },
        enumerable: true, configurable: true,
    });
}
tag(Location);
defGlobal('Location', Location);
const theLocation = Object.create(Location.prototype);
defProps(WP, {
    get location() { return theLocation; },
    set location(v) { navigateTo(str(v), false); },
});

/* ---------------- history ---------------- */

let historyState = null;
class History {
    constructor() { throw new TypeError('Illegal constructor'); }
    get length() { return N.historyLength(); }
    get state() { return historyState; }
    get scrollRestoration() { return 'auto'; }
    set scrollRestoration(v) {}
    go(d) { N.historyGo(Number(d) | 0); }
    back() { N.historyGo(-1); }
    forward() { N.historyGo(1); }
    pushState(state, title, url) { changeState(state, url, false); }
    replaceState(state, title, url) { changeState(state, url, true); }
}
function changeState(state, url, replace) {
    const s = structuredClone(state);
    let abs = N.docUrl(0);
    if (url !== undefined && url !== null) {
        const u = new URL(str(url), N.docUrl(1));
        const cur = new URL(abs);
        if (u.origin !== cur.origin || (u.protocol !== cur.protocol))
            throw domErr('The URL must have the same origin as the document', 'SecurityError');
        abs = u.href;
    }
    historyState = s;
    if (replace) N.historyReplace(abs); else N.historyPush(abs);
}
P.popState = function (url, state) {
    if (url) N.setDocUrl(url);
    historyState = state === undefined ? null : state;
    fire(G, new PopStateEvent('popstate', { state: historyState }));
};
tag(History);
defGlobal('History', History);
const theHistory = Object.create(History.prototype);
defProps(WP, { get history() { return theHistory; } });

/* ---------------- navigator, screen ---------------- */

/* Fixed strings only: nothing here describes the machine beyond what the
 * settings name (privacy). */
class Navigator { constructor() { throw new TypeError('Illegal constructor'); } }
const navProps = {
    get userAgent() { return settings.userAgent; },
    get appVersion() { return settings.userAgent.replace(/^Mozilla\//, ''); },
    get appName() { return 'Netscape'; },
    get appCodeName() { return 'Mozilla'; },
    get product() { return 'Gecko'; },
    get productSub() { return '20030107'; },
    get vendor() { return ''; },
    get vendorSub() { return ''; },
    get platform() { return 'MacPPC'; },
    get oscpu() { return 'PPC Mac OS 9'; },
    get language() { return settings.language; },
    get languages() { return Object.freeze([settings.language]); },
    get onLine() { return settings.online; },
    get cookieEnabled() { return settings.cookieEnabled; },
    get doNotTrack() { return settings.dnt ? '1' : null; },
    get globalPrivacyControl() { return settings.gpc; },
    get hardwareConcurrency() { return 1; },
    get maxTouchPoints() { return 0; },
    get webdriver() { return false; },
    get pdfViewerEnabled() { return false; },
    get plugins() { return Object.freeze([]); },
    get mimeTypes() { return Object.freeze([]); },
    javaEnabled() { return false; },
    sendBeacon() { return false; },
    vibrate() { return false; },
    registerProtocolHandler() {},
};
defProps(Navigator.prototype, navProps);
tag(Navigator);
defGlobal('Navigator', Navigator);
const theNavigator = Object.create(Navigator.prototype);
class Screen { constructor() { throw new TypeError('Illegal constructor'); } }
defProps(Screen.prototype, {
    get width() { return settings.screenWidth; },
    get height() { return settings.screenHeight; },
    get availWidth() { return settings.screenWidth; },
    get availHeight() { return settings.screenHeight - 20; },
    get availLeft() { return 0; },
    get availTop() { return 20; },
    get colorDepth() { return 24; },
    get pixelDepth() { return 24; },
    get orientation() { return { type: settings.screenWidth >= settings.screenHeight ? 'landscape-primary' : 'portrait-primary', angle: 0 }; },
});
tag(Screen);
defGlobal('Screen', Screen);
const theScreen = Object.create(Screen.prototype);
defProps(WP, {
    get navigator() { return theNavigator; },
    get clientInformation() { return theNavigator; },
    get screen() { return theScreen; },
});

/* ---------------- matchMedia ---------------- */

const mqlists = new Set();
class MediaQueryList extends EventTarget {
    constructor(q) {
        super();
        if (typeof q !== 'string') throw new TypeError('Illegal constructor');
        this._q = q;
        this._m = N.mediaMatches(q);
        this._onchange = null;
    }
    get media() { return this._q; }
    get matches() { return N.mediaMatches(this._q); }
    addListener(cb) { if (cb) { mqlists.add(this); this.addEventListener('change', cb); } }
    removeListener(cb) { this.removeEventListener('change', cb); }
    addEventListener(type, cb, opts) { mqlists.add(this); super.addEventListener(type, cb, opts); }
}
defineHandlers(MediaQueryList.prototype, ['change']);
tag(MediaQueryList);
defGlobal('MediaQueryList', MediaQueryList);
class MediaQueryListEvent extends Event {
    constructor(type, init = {}) { super(type, init); this._media = str(init.media ?? ''); this._matches = !!init.matches; }
    get media() { return this._media; }
    get matches() { return this._matches; }
}
defGlobal('MediaQueryListEvent', MediaQueryListEvent);
function checkMediaLists() {
    for (const l of mqlists) {
        const m = N.mediaMatches(l._q);
        if (m !== l._m) {
            l._m = m;
            fire(l, new MediaQueryListEvent('change', { media: l._q, matches: m }));
        }
    }
}

P.resized = function () {
    checkMediaLists();
    fireSimple(G, 'resize', false, false);
    observeResizes();
};
P.scrolled = function () {
    fireSimple(doc, 'scroll', true, false);
};

/* ---------------- performance ---------------- */

const marks = [];
class PerformanceEntry {
    constructor(name, type, start, duration, detail) { this._n = name; this._t = type; this._s = start; this._d = duration; this._detail = detail ?? null; }
    get name() { return this._n; }
    get entryType() { return this._t; }
    get startTime() { return this._s; }
    get duration() { return this._d; }
    get detail() { return this._detail; }
    toJSON() { return { name: this._n, entryType: this._t, startTime: this._s, duration: this._d }; }
}
class PerformanceMark extends PerformanceEntry {}
class PerformanceMeasure extends PerformanceEntry {}
const nowRel = () => N.now() - timeOriginNow;
const markTime = (m) => {
    if (typeof m === 'number') return m;
    for (let i = marks.length - 1; i >= 0; i--) if (marks[i].name === m && marks[i].entryType === 'mark') return marks[i].startTime;
    if (m === 'navigationStart' || m === 'fetchStart') return 0;
    throw domErr('No mark named ' + m, 'SyntaxError');
};
class Performance extends EventTarget {
    constructor() { throw new TypeError('Illegal constructor'); }
}
defProps(Performance.prototype, {
    now() { return Math.round(nowRel() * 10) / 10; },
    get timeOrigin() { return timeOrigin; },
    mark(name, opts) {
        const m = new PerformanceMark(str(name), 'mark', opts && opts.startTime !== undefined ? +opts.startTime : nowRel(), 0, opts && opts.detail);
        if (marks.length < 10000) marks.push(m);
        return m;
    },
    measure(name, a, b) {
        let start = 0, end = nowRel(), detail;
        if (a && typeof a === 'object') {
            if (a.start !== undefined) start = markTime(a.start);
            if (a.end !== undefined) end = markTime(a.end);
            if (a.duration !== undefined) end = start + +a.duration;
            detail = a.detail;
        } else {
            if (a !== undefined) start = markTime(a);
            if (b !== undefined) end = markTime(b);
        }
        const m = new PerformanceMeasure(str(name), 'measure', start, end - start, detail);
        if (marks.length < 10000) marks.push(m);
        return m;
    },
    clearMarks(name) { for (let i = marks.length - 1; i >= 0; i--) if (marks[i].entryType === 'mark' && (name === undefined || marks[i].name === name)) marks.splice(i, 1); },
    clearMeasures(name) { for (let i = marks.length - 1; i >= 0; i--) if (marks[i].entryType === 'measure' && (name === undefined || marks[i].name === name)) marks.splice(i, 1); },
    getEntries() { return marks.slice(); },
    getEntriesByType(t) { return marks.filter((m) => m.entryType === t); },
    getEntriesByName(n, t) { return marks.filter((m) => m.name === n && (t === undefined || m.entryType === t)); },
    clearResourceTimings() {},
    setResourceTimingBufferSize() {},
    get timing() {
        return { navigationStart: timeOrigin, fetchStart: timeOrigin, domLoading: timeOrigin, domInteractive: timeOrigin,
            domContentLoadedEventStart: timeOrigin, domContentLoadedEventEnd: timeOrigin, domComplete: timeOrigin,
            loadEventStart: timeOrigin, loadEventEnd: timeOrigin, responseStart: timeOrigin, responseEnd: timeOrigin,
            requestStart: timeOrigin, connectStart: timeOrigin, connectEnd: timeOrigin };
    },
    get navigation() { return { type: 0, redirectCount: 0, TYPE_NAVIGATE: 0, TYPE_RELOAD: 1, TYPE_BACK_FORWARD: 2 }; },
    toJSON() { return { timeOrigin }; },
});
tag(Performance);
for (const C of [Performance, PerformanceEntry, PerformanceMark, PerformanceMeasure]) defGlobal(C.name, C);
const thePerformance = Object.create(Performance.prototype);
defProps(WP, { get performance() { return thePerformance; } });
class PerformanceObserver {
    constructor(cb) { if (typeof cb !== 'function') throw new TypeError('PerformanceObserver: callback is not a function'); }
    observe() {}
    disconnect() {}
    takeRecords() { return []; }
    static get supportedEntryTypes() { return Object.freeze(['mark', 'measure']); }
}
defGlobal('PerformanceObserver', PerformanceObserver);

/* ---------------- structuredClone ---------------- */

function structuredClone(value, options) {
    const seen = new Map();
    let budget = 1000000;
    const clone = (v) => {
        if (v === null || (typeof v !== 'object' && typeof v !== 'function')) {
            if (typeof v === 'symbol') throw domErr('A symbol could not be cloned', 'DataCloneError');
            return v;
        }
        if (typeof v === 'function') throw domErr('A function could not be cloned', 'DataCloneError');
        if (seen.has(v)) return seen.get(v);
        if (--budget < 0) throw domErr('The value is too large to clone', 'DataCloneError');
        let out;
        if (Array.isArray(v)) {
            out = new Array(v.length);
            seen.set(v, out);
            for (const k of Object.keys(v)) out[k] = clone(v[k]);
            return out;
        }
        if (v instanceof Date) out = new Date(v.getTime());
        else if (v instanceof RegExp) out = new RegExp(v.source, v.flags);
        else if (v instanceof Boolean || v instanceof Number || v instanceof String) out = Object(v.valueOf());
        else if (v instanceof ArrayBuffer) out = v.slice(0);
        else if (ArrayBuffer.isView(v)) {
            const buf = clone(v.buffer);
            out = v instanceof DataView ? new DataView(buf, v.byteOffset, v.byteLength) : new v.constructor(buf, v.byteOffset, v.length);
        } else if (v instanceof Map) {
            out = new Map();
            seen.set(v, out);
            for (const [a, b] of v) out.set(clone(a), clone(b));
            return out;
        } else if (v instanceof Set) {
            out = new Set();
            seen.set(v, out);
            for (const a of v) out.add(clone(a));
            return out;
        } else if (v instanceof Error) {
            const C = { EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError }[v.name] || Error;
            out = new C(v.message);
            if (v.stack !== undefined) out.stack = String(v.stack);
        } else if (v instanceof Blob) {
            out = v;
        } else if (isNode(v) || v instanceof EventTarget || v instanceof WeakMap || v instanceof WeakSet ||
            v instanceof Promise || (typeof WeakRef === 'function' && v instanceof WeakRef)) {
            throw domErr('The object could not be cloned', 'DataCloneError');
        } else {
            out = {};
            seen.set(v, out);
            for (const k of Object.keys(v)) out[k] = clone(v[k]);
            return out;
        }
        seen.set(v, out);
        return out;
    };
    return clone(value);
}
defGlobal('structuredClone', structuredClone);

/* ---------------- URL, URLSearchParams ---------------- */

/* The parser is N.urlParse (src/js/url.c, the WHATWG parser of src/url):
 * [href, protocol, username, password, hostname, port, pathname, search,
 * hash] or null. */
const specialPorts = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21' };
function parseUrl(input, base) {
    const r = base === undefined ? N.urlParse(input) : N.urlParse(input, base);
    if (!r) throw new TypeError('Invalid URL: ' + input);
    return r;
}
class URL {
    constructor(url, base) {
        url = str(url);
        if (base !== undefined) base = str(base instanceof URL ? base.href : base);
        if (base !== undefined) parseUrl(base);
        this._p = parseUrl(url, base);
        this._sp = null;
    }
    static canParse(url, base) { try { new URL(url, base); return true; } catch (e) { return false; } }
    static parse(url, base) { try { return new URL(url, base); } catch (e) { return null; } }
    static createObjectURL(blob) { return blobUrls.create(blob); }
    static revokeObjectURL(u) { blobUrls.revoke(str(u)); }
    _set(p) { this._p = p; if (this._sp) this._sp._load(this._p[7]); }
    _rebuild(parts) {
        const [, protocol, username, password, hostname, port, pathname, search, hash] = parts;
        let s = protocol;
        const hasHost = hostname !== '' || protocol === 'file:' || specialPorts[protocol] !== undefined;
        if (hasHost) {
            s += '//';
            if (username || password) s += username + (password ? ':' + password : '') + '@';
            s += hostname + (port ? ':' + port : '');
        }
        s += pathname + search + hash;
        const r = N.urlParse(s);
        if (r) this._set(r);
    }
    get href() { return this._p[0]; }
    set href(v) { this._set(parseUrl(str(v))); }
    toString() { return this._p[0]; }
    toJSON() { return this._p[0]; }
    get origin() { const o = N.originOf(this._p[0]); return o === 'file://' ? 'null' : o; }
    get protocol() { return this._p[1]; }
    set protocol(v) {
        v = str(v).replace(/:.*$/, '').toLowerCase();
        if (!/^[a-z][a-z0-9+.-]*$/.test(v)) return;
        const special = specialPorts[this._p[1]] !== undefined || this._p[1] === 'file:';
        const toSpecial = specialPorts[v + ':'] !== undefined || v === 'file';
        if (special !== toSpecial) return;
        const p = this._p.slice(); p[1] = v + ':';
        if (p[5] === specialPorts[p[1]]) p[5] = '';
        this._rebuild(p);
    }
    get username() { return this._p[2]; }
    set username(v) { if (!this._p[4] || this._p[1] === 'file:') return; const p = this._p.slice(); p[2] = encodeURIComponent(str(v)); this._rebuild(p); }
    get password() { return this._p[3]; }
    set password(v) { if (!this._p[4] || this._p[1] === 'file:') return; const p = this._p.slice(); p[3] = encodeURIComponent(str(v)); this._rebuild(p); }
    get host() { return this._p[4] + (this._p[5] ? ':' + this._p[5] : ''); }
    set host(v) {
        const m = /^([^:/?#]*)(?::(\d*))?/.exec(str(v));
        if (!m || !m[1]) return;
        const p = this._p.slice(); p[4] = m[1]; if (m[2] !== undefined) p[5] = m[2] === specialPorts[p[1]] ? '' : m[2];
        this._rebuild(p);
    }
    get hostname() { return this._p[4]; }
    set hostname(v) { const h = /^[^:/?#]*/.exec(str(v))[0]; if (!h) return; const p = this._p.slice(); p[4] = h; this._rebuild(p); }
    get port() { return this._p[5]; }
    set port(v) {
        if (!this._p[4] || this._p[1] === 'file:') return;
        const d = /^\d*/.exec(str(v))[0];
        if (d === '' && str(v) !== '') return;
        const p = this._p.slice(); p[5] = d === '' || d === specialPorts[p[1]] ? '' : String(parseInt(d, 10));
        if (+p[5] > 65535) return;
        this._rebuild(p);
    }
    get pathname() { return this._p[6]; }
    set pathname(v) {
        v = str(v);
        const p = this._p.slice();
        p[6] = specialPorts[p[1]] !== undefined || p[1] === 'file:' ? (v.startsWith('/') ? v : '/' + v) : v;
        p[6] = p[6].replace(/[?#]/g, (c) => (c === '?' ? '%3F' : '%23'));
        this._rebuild(p);
    }
    get search() { return this._p[7] === '?' ? '' : this._p[7]; }
    set search(v) {
        v = str(v);
        const p = this._p.slice();
        p[7] = v === '' ? '' : '?' + v.replace(/^\?/, '').replace(/#/g, '%23');
        this._rebuild(p);
    }
    get searchParams() {
        if (!this._sp) { this._sp = new URLSearchParams(this.search); this._sp._url = this; }
        return this._sp;
    }
    get hash() { return this._p[8] === '#' ? '' : this._p[8]; }
    set hash(v) { v = str(v); const p = this._p.slice(); p[8] = v === '' ? '' : '#' + v.replace(/^#/, ''); this._rebuild(p); }
}
tag(URL);
defGlobal('URL', URL);
defGlobal('webkitURL', URL);

const formDecode = (s) => {
    s = s.replace(/\+/g, ' ');
    try { return decodeURIComponent(s); } catch (e) {
        return s.replace(/%([0-9a-fA-F]{2})/g, (m, h) => String.fromCharCode(parseInt(h, 16)));
    }
};
class URLSearchParams {
    constructor(init = '') {
        this._l = [];
        this._url = null;
        if (init instanceof URLSearchParams) this._l = init._l.map((p) => p.slice());
        else if (init !== null && typeof init === 'object') {
            if (typeof init[Symbol.iterator] === 'function') {
                for (const pair of init) {
                    const a = Array.from(pair);
                    if (a.length !== 2) throw new TypeError('URLSearchParams: each pair must have two items');
                    this._l.push([str(a[0]).toWellFormed(), str(a[1]).toWellFormed()]);
                }
            } else for (const k of Object.keys(init)) this._l.push([k.toWellFormed(), str(init[k]).toWellFormed()]);
        } else this._load(str(init));
    }
    _load(s) {
        this._l = [];
        s = s.replace(/^\?/, '');
        for (const part of s.split('&')) {
            if (!part) continue;
            const i = part.indexOf('=');
            this._l.push(i < 0 ? [formDecode(part), ''] : [formDecode(part.slice(0, i)), formDecode(part.slice(i + 1))]);
        }
    }
    _update() {
        if (!this._url) return;
        const s = this.toString();
        const u = this._url;
        const p = u._p.slice();
        p[7] = s ? '?' + s : '';
        const sp = u._sp; u._sp = null;
        u._rebuild(p);
        u._sp = sp;
    }
    get size() { return this._l.length; }
    append(k, v) { this._l.push([str(k), str(v)]); this._update(); }
    delete(k, v) { k = str(k); this._l = this._l.filter((p) => p[0] !== k || (v !== undefined && p[1] !== str(v))); this._update(); }
    get(k) { k = str(k); const p = this._l.find((x) => x[0] === k); return p ? p[1] : null; }
    getAll(k) { k = str(k); return this._l.filter((x) => x[0] === k).map((x) => x[1]); }
    has(k, v) { k = str(k); return this._l.some((x) => x[0] === k && (v === undefined || x[1] === str(v))); }
    set(k, v) {
        k = str(k); v = str(v);
        const i = this._l.findIndex((x) => x[0] === k);
        if (i < 0) this._l.push([k, v]);
        else { this._l[i][1] = v; this._l = this._l.filter((x, j) => j <= i || x[0] !== k); }
        this._update();
    }
    sort() {
        const cmp = (a, b) => { for (let i = 0; i < Math.min(a.length, b.length); i++) { const d = a.charCodeAt(i) - b.charCodeAt(i); if (d) return d; } return a.length - b.length; };
        this._l = this._l.map((p, i) => [p, i]).sort((x, y) => cmp(x[0][0], y[0][0]) || x[1] - y[1]).map((x) => x[0]);
        this._update();
    }
    forEach(fn, thisArg) { for (const [k, v] of this._l.slice()) fn.call(thisArg, v, k, this); }
    keys() { return this._l.map((p) => p[0])[Symbol.iterator](); }
    values() { return this._l.map((p) => p[1])[Symbol.iterator](); }
    entries() { return this._l.map((p) => p.slice())[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return formEncode(this._l); }
}
tag(URLSearchParams);
defGlobal('URLSearchParams', URLSearchParams);

/* ---------------- TextEncoder, TextDecoder ---------------- */

class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s = '') { return N.utf8Encode(str(s).toWellFormed()); }
    encodeInto(s, dest) {
        if (!(dest instanceof Uint8Array)) throw new TypeError('encodeInto: the destination is not a Uint8Array');
        s = str(s).toWellFormed();
        let read = 0, written = 0;
        for (const ch of s) {
            const b = N.utf8Encode(ch);
            if (written + b.length > dest.length) break;
            dest.set(b, written);
            written += b.length;
            read += ch.length;
        }
        return { read, written };
    }
}
tag(TextEncoder);
defGlobal('TextEncoder', TextEncoder);
const utf8Labels = ['unicode-1-1-utf-8', 'unicode11utf8', 'unicode20utf8', 'utf-8', 'utf8', 'x-unicode20utf8'];
const latin1Labels = ['ansi_x3.4-1968', 'ascii', 'cp1252', 'cp819', 'csisolatin1', 'ibm819', 'iso-8859-1', 'iso-ir-100',
    'iso8859-1', 'iso88591', 'iso_8859-1', 'iso_8859-1:1987', 'l1', 'latin1', 'us-ascii', 'windows-1252', 'x-cp1252'];
const cp1252 = '€\x81‚ƒ„…†‡ˆ‰Š‹Œ\x8dŽ\x8f\x90‘’“”•–—˜™š›œ\x9džŸ';
class TextDecoder {
    constructor(label = 'utf-8', opts = {}) {
        const l = str(label).trim().toLowerCase();
        if (utf8Labels.includes(l)) this._enc = 'utf-8';
        else if (latin1Labels.includes(l)) this._enc = 'windows-1252';
        else if (l === 'utf-16le' || l === 'utf-16') this._enc = 'utf-16le';
        else if (l === 'utf-16be') this._enc = 'utf-16be';
        else throw new RangeError('The encoding label is not supported: ' + label);
        this._fatal = !!(opts && opts.fatal);
        this._ignoreBOM = !!(opts && opts.ignoreBOM);
        this._pending = null;
        this._bomSeen = false;
    }
    get encoding() { return this._enc; }
    get fatal() { return this._fatal; }
    get ignoreBOM() { return this._ignoreBOM; }
    decode(input, opts) {
        let bytes;
        if (input === undefined || input === null) bytes = new Uint8Array(0);
        else if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
        else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
        else throw new TypeError('decode: the input is not a BufferSource');
        const stream = !!(opts && opts.stream);
        if (this._pending) { const b = new Uint8Array(this._pending.length + bytes.length); b.set(this._pending); b.set(bytes, this._pending.length); bytes = b; this._pending = null; }
        if (this._enc === 'utf-8') {
            if (stream) {
                /* keep an incomplete trailing sequence for the next call */
                let cut = bytes.length;
                for (let i = Math.max(0, bytes.length - 3); i < bytes.length; i++) {
                    const b = bytes[i];
                    const need = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : b >= 0xc0 ? 2 : 0;
                    if (need && i + need > bytes.length) { cut = i; break; }
                }
                if (cut < bytes.length) { this._pending = bytes.slice(cut); bytes = bytes.subarray(0, cut); }
            }
            const skipBom = this._ignoreBOM || this._bomSeen;
            const s = N.utf8Decode(bytes, this._fatal, skipBom);
            if (bytes.length) this._bomSeen = stream;
            if (!stream) this._bomSeen = false;
            return s;
        }
        if (this._enc === 'windows-1252') {
            let s = '';
            for (const b of bytes) s += b >= 0x80 && b < 0xa0 ? cp1252[b - 0x80] : String.fromCharCode(b);
            return s;
        }
        const le = this._enc === 'utf-16le';
        let n = bytes.length & ~1;
        if (stream && bytes.length & 1) this._pending = bytes.slice(n);
        let s = '';
        let i = 0;
        if (!this._ignoreBOM && n >= 2 && ((le && bytes[0] === 0xff && bytes[1] === 0xfe) || (!le && bytes[0] === 0xfe && bytes[1] === 0xff))) i = 2;
        for (; i < n; i += 2) s += String.fromCharCode(le ? bytes[i] | (bytes[i + 1] << 8) : (bytes[i] << 8) | bytes[i + 1]);
        if (!stream && bytes.length & 1) { if (this._fatal) throw new TypeError('The data is not valid UTF-16'); s += '�'; }
        return this._fatal || s.isWellFormed() ? s : s.toWellFormed();
    }
}
tag(TextDecoder);
defGlobal('TextDecoder', TextDecoder);

/* ---------------- Storage ---------------- */

/* localStorage persists through the embedder (storageLoad/storageSave,
 * one JSON string per origin); sessionStorage lives as long as the
 * document. Both enforce the quota from the settings on the UTF-16 length
 * of keys and values. */
class Storage {
    constructor() { throw new TypeError('Illegal constructor'); }
}
const storageData = new WeakMap();
const storageSize = (m) => { let n = 0; for (const [k, v] of m) n += k.length + v.length; return n; };
function storageSave(s) { const d = storageData.get(s); if (d.persist) N.storageSave(JSON.stringify([...d.map])); }
defProps(Storage.prototype, {
    get length() { return storageData.get(this).map.size; },
    key(i) { const k = [...storageData.get(this).map.keys()][i >>> 0]; return k === undefined ? null : k; },
    getItem(k) { const v = storageData.get(this).map.get(str(k)); return v === undefined ? null : v; },
    setItem(k, v) {
        k = str(k); v = str(v);
        const d = storageData.get(this);
        const old = d.map.get(k);
        if (old === v) return;
        const size = storageSize(d.map) - (old === undefined ? 0 : k.length + old.length) + k.length + v.length;
        if (size * 2 > settings.quota) throw domErr('The storage quota has been exceeded', 'QuotaExceededError');
        d.map.set(k, v);
        storageSave(this);
    },
    removeItem(k) { const d = storageData.get(this); if (d.map.delete(str(k))) storageSave(this); },
    clear() { const d = storageData.get(this); if (d.map.size) { d.map.clear(); storageSave(this); } },
});
tag(Storage);
defGlobal('Storage', Storage);
const storageHandler = {
    get(t, p) { if (typeof p !== 'string' || p in t) return Reflect.get(t, p, t); const v = storageData.get(t).map.get(p); return v; },
    set(t, p, v) { if (typeof p !== 'string' || p in Storage.prototype) return Reflect.set(t, p, v, t); t.setItem(p, v); return true; },
    has(t, p) { return typeof p === 'string' && storageData.get(t).map.has(p) || p in t; },
    deleteProperty(t, p) { if (typeof p === 'string') t.removeItem(p); return true; },
    ownKeys(t) { return [...storageData.get(t).map.keys()]; },
    getOwnPropertyDescriptor(t, p) {
        const v = typeof p === 'string' ? storageData.get(t).map.get(p) : undefined;
        return v === undefined ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
    },
};
function makeStorage(persist) {
    const s = Object.create(Storage.prototype);
    const map = new Map();
    if (persist) {
        try {
            const raw = N.storageLoad();
            if (raw) for (const [k, v] of JSON.parse(raw)) if (typeof k === 'string' && typeof v === 'string') map.set(k, v);
        } catch (e) { /* damaged data: start empty */ }
    }
    const data = { map, persist };
    const p = new Proxy(s, storageHandler);
    storageData.set(s, data);
    storageData.set(p, data); /* methods see the proxy as this */
    return p;
}
let localStore = null, sessionStore = null;
const storageAllowed = () => N.docUrl(2) !== 'null' || /^file:/.test(N.docUrl(0));
defProps(WP, {
    get localStorage() {
        if (!storageAllowed()) throw domErr('Storage is not available for this document', 'SecurityError');
        return localStore || (localStore = makeStorage(true));
    },
    get sessionStorage() {
        if (!storageAllowed()) throw domErr('Storage is not available for this document', 'SecurityError');
        return sessionStore || (sessionStore = makeStorage(false));
    },
});

/* ---------------- misc globals ---------------- */

defGlobal('isSecureContext', G.isSecureContext);
class BarProp { get visible() { return true; } }
const bar = Object.create(BarProp.prototype);
defProps(WP, {
    get locationbar() { return bar; }, get menubar() { return bar; }, get personalbar() { return bar; },
    get scrollbars() { return bar; }, get statusbar() { return bar; }, get toolbar() { return bar; },
    get visualViewport() { return null; },
    get customElements() { return customElementRegistry(); },
});
