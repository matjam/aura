
/* ---------------- window ---------------- */

const WP = Window.prototype;
const settings = N.settings();
let winName = null; /* window.name once script has set it */
defProps(WP, {
    get window() { return G; },
    get self() { return G; },
    set self(v) { defGlobal('self', v); },
    get frames() { return G; },
    get parent() { return wrapWindow(N.windowRel(1)); },
    get top() { return wrapWindow(N.windowRel(2)); },
    get opener() { return null; },
    set opener(v) {},
    get frameElement() { return N.frameElement(); },
    get length() { return N.frameCount(); },
    get closed() { return false; },
    /* The window's name: what script last set, else the name attribute of
     * the frame element holding the document ("" for the top). */
    get name() { return winName !== null ? winName : N.frameName(); },
    set name(v) { winName = String(v); },
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
        if (target === '_self') { G.location.assign(abs); return G; }
        if (target === '_top' || target === '_parent') {
            const w = N.windowRel(target === '_top' ? 2 : 1);
            if (w === G) { G.location.assign(abs); return G; }
            if (!N.navigateWindow(w, abs)) throw domErr('This document may not navigate that window', 'SecurityError');
            return wrapWindow(w);
        }
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
        /* The caller may be another document of the same origin (this
         * function is reached through its window object): the message
         * then comes from that document. */
        const src = N.enteredWindow();
        if (src !== G && o === '/' && N.windowInfo(src, 4) !== N.docUrl(2)) return;
        /* its ports are known to its own realm */
        const ends = src === G ? transferOf(origin, arguments[2]) : N.transferFrom(src, origin, arguments[2]);
        P.receiveMessage(data, src, src === G ? N.docUrl(2) : N.windowInfo(src, 4), ends);
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

/* animation frames (timers are in prelude_web.js) */
defGlobal('requestAnimationFrame', function requestAnimationFrame(cb) {
    if (typeof cb !== 'function') throw new TypeError('requestAnimationFrame: callback is not a function');
    return N.requestAnimationFrame((t) => cb.call(G, t - timeOriginNow));
});
defGlobal('cancelAnimationFrame', function cancelAnimationFrame(id) { N.cancelAnimationFrame(Number(id) >>> 0); });

/* ---------------- location ---------------- */

class DOMStringList {
    constructor() { throw new TypeError('Illegal constructor'); }
    item(i) { i >>>= 0; return i < this.length ? this[i] : null; }
    contains(s) { return Array.prototype.includes.call(this, str(s)); }
    [Symbol.iterator]() { return Array.prototype[Symbol.iterator].call(this); }
}
tag(DOMStringList);
defGlobal('DOMStringList', DOMStringList);
const originList = (items) => {
    const l = Object.create(DOMStringList.prototype);
    items.forEach((v, i) => Object.defineProperty(l, i, { value: v, enumerable: true }));
    Object.defineProperty(l, 'length', { value: items.length });
    return Object.freeze(l);
};
class Location {
    constructor() { throw new TypeError('Illegal constructor'); }
    get href() { return N.docUrl(0); }
    set href(v) { navigateTo(str(v), false); }
    assign(v) { navigateTo(str(v), false); }
    replace(v) { navigateTo(str(v), true); }
    reload() { N.reload(); }
    toString() { return N.docUrl(0); }
    /* The origins of the documents this one is nested in, the parent's first. */
    get ancestorOrigins() {
        const out = [];
        let g = G;
        for (let i = 0; i < 64; i++) {
            const p = N.windowInfo(g, 1);
            if (p === null || p === g) break;
            out.push(N.windowInfo(p, 4));
            g = p;
        }
        return originList(out);
    }
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
    sendBeacon(url, data) { if (!arguments.length) throw new TypeError('sendBeacon: 1 argument required'); return sendBeacon(url, data); },
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

/* ---------------- windows of other documents ----------------
 * N.frameWindow, N.windowRel and the like return the global object of
 * another document of the page. wrapWindow turns one into what page
 * script may hold: the global itself for a document of the same origin,
 * else an object with the members the standard allows across origins
 * (postMessage, location as a setter, closed, parent, top, frames,
 * length, indexed and named frames, focus, blur, close). Anything else throws
 * SecurityError. The object keeps the global in a closure only. */
const crossWindows = new WeakMap();
const securityErr = () => domErr('Blocked a frame from accessing a cross-origin frame', 'SecurityError');
function crossOriginWindow(g) {
    const noop = () => {};
    const loc = Object.freeze(Object.create(null, {
        href: { set(v) { setLoc(v); }, get() { throw securityErr(); }, enumerable: true },
        replace: { value: (v) => setLoc(v), enumerable: true },
    }));
    const setLoc = (v) => {
        if (!N.navigateWindow(g, resolveUrl(str(v)))) throw domErr('This document may not navigate that window', 'SecurityError');
    };
    const post = function postMessage(data, origin, transfer) {
        if (!arguments.length) throw new TypeError('postMessage: 1 argument required');
        let o = typeof origin === 'object' && origin ? (origin.targetOrigin === undefined ? '/' : str(origin.targetOrigin))
            : str(origin === undefined ? '/' : origin);
        if (o === '/') o = N.docUrl(2);
        else if (o !== '*') {
            let u;
            try { u = new URL(o); } catch (e) { throw domErr("postMessage: invalid target origin '" + o + "'", 'SyntaxError'); }
            o = u.origin;
        }
        const cloned = structuredClone(data);
        N.postMessageTo(g, cloned, o, transferOf(origin, transfer));
    };
    let self;
    const get = (p) => {
        switch (p) {
        case 'postMessage': return post;
        case 'location': return loc;
        case 'closed': return N.windowInfo(g, 3);
        case 'parent': return wrapWindow(N.windowInfo(g, 1));
        case 'top': return wrapWindow(N.windowInfo(g, 2));
        case 'window': case 'self': case 'frames': return self;
        case 'length': return N.windowInfo(g, 0);
        case 'opener': return null;
        case 'focus': case 'blur': case 'close': return noop;
        case 'then': return undefined;
        }
        if (typeof p === 'symbol') {
            if (p === Symbol.toStringTag) return 'Object';
            return undefined;
        }
        if (isIndex(p)) {
            const w = N.windowFrameAt(g, +p);
            if (w !== null) return wrapWindow(w);
        } else {
            /* a child frame by its name (widgets find a sibling frame
             * through their parent this way) */
            const w = N.windowFrameNamed(g, p);
            if (w !== null) return wrapWindow(w);
        }
        throw securityErr();
    };
    self = new Proxy(Object.create(null), {
        get(t, p) { return get(p); },
        set(t, p, v) { if (p === 'location') { setLoc(v); return true; } throw securityErr(); },
        has(t, p) { throw securityErr(); },
        ownKeys() { return []; },
        getOwnPropertyDescriptor(t, p) { if (p === 'then' || typeof p === 'symbol') return undefined; throw securityErr(); },
        defineProperty() { throw securityErr(); },
        deleteProperty() { throw securityErr(); },
        getPrototypeOf() { return null; },
        setPrototypeOf() { throw securityErr(); },
        preventExtensions() { throw securityErr(); },
    });
    return self;
}
function wrapWindow(g) {
    if (g === null || g === undefined) return null;
    if (g === G || N.windowAccess(g)) return g;
    let w = crossWindows.get(g);
    if (!w) { w = crossOriginWindow(g); crossWindows.set(g, w); }
    return w;
}
/* The window and the document of the frame in an iframe, frame, object or
 * embed element. */
const frameWindowOf = (el) => wrapWindow(N.frameWindow(el));
const frameDocumentOf = (el) => {
    const g = N.frameWindow(el);
    return g !== null && N.windowAccess(g) ? g.document : null;
};
/* A message for this window from the document whose global is src (data
 * is the sender's value, or its clone when the sender is of another
 * origin): cloned here, in this realm, and delivered as a task. */
P.receiveMessage = function (data, src, srcOrigin, ends) {
    const cloned = structuredClone(data);
    detachAll(ends && ends.buffers);
    N.queueTask(() => fire(G, new MessageEvent('message', {
        data: cloned, origin: srcOrigin, source: wrapWindow(src), ports: adoptEnds(ends) })));
};
/* The window's indexed and named frames: own accessor properties, set
 * again whenever the frame list changes. A name that the window already
 * has (a member, a global variable) is left alone. */
let frameProps = [];
P.framesChanged = function () {
    for (const k of frameProps) {
        const d = Object.getOwnPropertyDescriptor(G, k);
        if (d && d.get && d.get.frameIndex !== undefined) delete G[k];
    }
    frameProps = [];
    const names = N.frameNames();
    const def = (k, i) => {
        const get = () => wrapWindow(N.frameWindow(i));
        get.frameIndex = i;
        Object.defineProperty(G, k, {
            get, set(v) { Object.defineProperty(G, k, { value: v, writable: true, enumerable: true, configurable: true }); },
            enumerable: false, configurable: true,
        });
        frameProps.push(k);
    };
    for (let i = 0; i < names.length; i++) def(String(i), i);
    for (let i = 0; i < names.length; i++) if (names[i] && !(names[i] in G)) def(names[i], i);
};

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
