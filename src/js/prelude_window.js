
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
    /* The window on its screen, as the shell reports it (screenGeom:
     * page/screen.h); a viewport at the screen's top left without. */
    get outerWidth() { const g = N.screenGeom(); return g ? g[8] : N.viewport()[0]; },
    get outerHeight() { const g = N.screenGeom(); return g ? g[9] : N.viewport()[1]; },
    get scrollX() { return N.viewport()[2]; },
    get scrollY() { return N.viewport()[3]; },
    get pageXOffset() { return N.viewport()[2]; },
    get pageYOffset() { return N.viewport()[3]; },
    get screenX() { const g = N.screenGeom(); return g ? g[6] : 0; },
    get screenY() { const g = N.screenGeom(); return g ? g[7] : 0; },
    get screenLeft() { const g = N.screenGeom(); return g ? g[6] : 0; },
    get screenTop() { const g = N.screenGeom(); return g ? g[7] : 0; },
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
        pushEntry(null, abs, false);
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

/* Each entry of this document has a key (0: the one it loaded in); the
 * browser keeps the key with the entry and hands it back when it moves to
 * that entry (P.popState). The states are kept here, the latest 64. */
let historyState = null;
let historyKey = 0, historyNextKey = 1;
const historyStates = new Map();
function pushEntry(state, abs, replace) {
    if (!replace) historyKey = historyNextKey++;
    historyState = state;
    historyStates.delete(historyKey);
    historyStates.set(historyKey, state);
    if (historyStates.size > 64) historyStates.delete(historyStates.keys().next().value);
    if (replace) N.historyReplace(abs, historyKey); else N.historyPush(abs, historyKey);
}
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
    pushEntry(s, abs, replace);
}
P.popState = function (url, key) {
    const old = N.docUrl(0);
    if (url) N.setDocUrl(url);
    historyKey = key >>> 0;
    const st = historyStates.get(historyKey);
    historyState = st === undefined ? null : st;
    fire(G, new PopStateEvent('popstate', { state: historyState }));
    const strip = (u) => u.replace(/#.*$/, '');
    if (url && url !== old && strip(url) === strip(old))
        N.queueTask(() => fire(G, new HashChangeEvent('hashchange', { oldURL: old, newURL: url })));
};
tag(History);
defGlobal('History', History);
const theHistory = Object.create(History.prototype);
defProps(WP, { get history() { return theHistory; } });

/* ---------------- navigator, screen ---------------- */

/* Fixed strings only: nothing here describes the machine beyond what the
 * settings name (privacy). */
class Navigator { constructor() { throw new TypeError('Illegal constructor'); } }
/* vendor and platform agree with the user agent the user chose
 * (Preferences > Network > Identify as): Chrome's vendor is Google's,
 * Safari's Apple's, and the platform is the one the string names. */
const uaVendor = (ua) => (/ Chrome\/\d/.test(ua) ? 'Google Inc.' : / Version\/[\d.]+.* Safari\//.test(ua) ? 'Apple Computer, Inc.' : '');
const uaPlatform = (ua, own) => (/PPC Mac OS/.test(ua) ? 'MacPPC' : /Mac OS X/.test(ua) ? 'MacIntel' :
    /Linux x86_64/.test(ua) ? 'Linux x86_64' : /Linux aarch64/.test(ua) ? 'Linux aarch64' : /Windows/.test(ua) ? 'Win32' : own);
const navProps = {
    get userAgent() { return settings.userAgent; },
    get appVersion() { return settings.userAgent.replace(/^Mozilla\//, ''); },
    get appName() { return 'Netscape'; },
    get appCodeName() { return 'Mozilla'; },
    get product() { return 'Gecko'; },
    get productSub() { return '20030107'; },
    get vendor() { return uaVendor(settings.userAgent); },
    get vendorSub() { return ''; },
    get platform() { return uaPlatform(settings.userAgent, settings.platform); },
    get oscpu() { return settings.oscpu; },
    get language() { return settings.language; },
    get languages() { return Object.freeze([settings.language]); },
    get onLine() { return settings.online; },
    get cookieEnabled() { return settings.cookieEnabled; },
    get storage() { return theStorageManager; },
    get doNotTrack() { return settings.dnt ? '1' : null; },
    get globalPrivacyControl() { return settings.gpc; },
    get hardwareConcurrency() { return 1; },
    get maxTouchPoints() { return 0; },
    get webdriver() { return false; },
    get pdfViewerEnabled() { return false; },
    get plugins() { return thePlugins; },
    get mimeTypes() { return theMimeTypes; },
    javaEnabled() { return false; },
    sendBeacon(url, data) { if (!arguments.length) throw new TypeError('sendBeacon: 1 argument required'); return sendBeacon(url, data); },
    vibrate() { return false; },
    registerProtocolHandler() {},
};
/* Aura has no plugins: both lists are empty, but they are the lists a
 * page expects (item, namedItem), not arrays. */
class PluginArray {
    constructor() { throw new TypeError('Illegal constructor'); }
}
class MimeTypeArray {
    constructor() { throw new TypeError('Illegal constructor'); }
}
for (const C of [PluginArray, MimeTypeArray]) {
    defProps(C.prototype, {
        get length() { return 0; },
        item() { return null; },
        namedItem() { return null; },
        [Symbol.iterator]() { return [][Symbol.iterator](); },
    });
    tag(C);
    defGlobal(C.name, C);
}
defProps(PluginArray.prototype, { refresh() {} });
const thePlugins = Object.create(PluginArray.prototype);
const theMimeTypes = Object.create(MimeTypeArray.prototype);
defProps(Navigator.prototype, navProps);
tag(Navigator);
defGlobal('Navigator', Navigator);
const theNavigator = Object.create(Navigator.prototype);
class Screen { constructor() { throw new TypeError('Illegal constructor'); } }
defProps(Screen.prototype, {
    get width() { const g = N.screenGeom(); return g ? g[0] : settings.screenWidth; },
    get height() { const g = N.screenGeom(); return g ? g[1] : settings.screenHeight; },
    get availWidth() { const g = N.screenGeom(); return g ? g[4] : settings.screenWidth; },
    get availHeight() { const g = N.screenGeom(); return g ? g[5] : settings.screenHeight - 20; },
    get availLeft() { const g = N.screenGeom(); return g ? g[2] : 0; },
    get availTop() { const g = N.screenGeom(); return g ? g[3] : 20; },
    get colorDepth() { return 24; },
    get pixelDepth() { return 24; },
    get orientation() { return { type: this.width >= this.height ? 'landscape-primary' : 'portrait-primary', angle: 0 }; },
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
{
    /* An onchange handler alone also watches the query. */
    const d = Object.getOwnPropertyDescriptor(MediaQueryList.prototype, 'onchange');
    Object.defineProperty(MediaQueryList.prototype, 'onchange', {
        configurable: true, enumerable: true, get: d.get,
        set(v) { if (v) mqlists.add(this); d.set.call(this, v); },
    });
}
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

P.mediaChanged = function () { checkMediaLists(); };
P.resized = function () {
    checkMediaLists();
    fireSimple(G, 'resize', false, false);
    observeResizes();
};
P.scrolled = function () {
    fireSimple(doc, 'scroll', true, false);
};
/* An element the user scrolled: scroll at it, not bubbling (CSSOM View). */
P.elementScrolled = function (el) {
    if (!el || !el.isConnected) return;
    if (traceScroll) {
        const s = N.elScroll(el);
        N.print(0, '[trace] scroll event at ' + traceName(el) + (s ? ' at ' + traceInt(s[0]) + ',' + traceInt(s[1]) +
                   ' of ' + traceInt(s[2]) + 'x' + traceInt(s[3]) : ''));
    }
    fireSimple(el, 'scroll', false, false);
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
/* The document's navigation entry. Its document milestones are real
 * (navTimes); the network phases all read 0, the time origin, since the
 * script's clock starts when the document does. */
class PerformanceResourceTiming extends PerformanceEntry {}
class PerformanceNavigationTiming extends PerformanceResourceTiming {
    constructor() { throw new TypeError('Illegal constructor'); }
}
const navEntryProps = {
    get name() { return N.docUrl(0); },
    get entryType() { return 'navigation'; },
    get startTime() { return 0; },
    get duration() { return navTimes.loadEnd; },
    get initiatorType() { return 'navigation'; },
    get nextHopProtocol() { return 'http/1.1'; },
    get type() { return 'navigate'; },
    get redirectCount() { return 0; },
    get domInteractive() { return navTimes.interactive; },
    get domContentLoadedEventStart() { return navTimes.contentLoaded; },
    get domContentLoadedEventEnd() { return navTimes.contentLoadedEnd; },
    get domComplete() { return navTimes.complete; },
    get loadEventStart() { return navTimes.complete; },
    get loadEventEnd() { return navTimes.loadEnd; },
    get serverTiming() { return Object.freeze([]); },
    toJSON() { const o = {}; for (const k of navEntryNames) o[k] = this[k]; return o; },
};
const navEntryZero = ['workerStart', 'redirectStart', 'redirectEnd', 'fetchStart', 'domainLookupStart', 'domainLookupEnd',
    'connectStart', 'secureConnectionStart', 'connectEnd', 'requestStart', 'responseStart', 'responseEnd', 'transferSize',
    'encodedBodySize', 'decodedBodySize', 'unloadEventStart', 'unloadEventEnd', 'activationStart'];
for (const k of navEntryZero) Object.defineProperty(navEntryProps, k, { get() { return 0; }, enumerable: true, configurable: true });
const navEntryNames = ['name', 'entryType', 'startTime', 'duration', 'initiatorType', 'nextHopProtocol', ...navEntryZero.slice(0, 15),
    'serverTiming', 'unloadEventStart', 'unloadEventEnd', 'domInteractive', 'domContentLoadedEventStart',
    'domContentLoadedEventEnd', 'domComplete', 'loadEventStart', 'loadEventEnd', 'type', 'redirectCount', 'activationStart'];
defProps(PerformanceNavigationTiming.prototype, navEntryProps);
const theNavEntry = Object.create(PerformanceNavigationTiming.prototype);
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
    getEntries() { return [theNavEntry, ...marks]; },
    getEntriesByType(t) { return t === 'navigation' ? [theNavEntry] : marks.filter((m) => m.entryType === t); },
    getEntriesByName(n, t) {
        const r = marks.filter((m) => m.name === n && (t === undefined || m.entryType === t));
        if (n === theNavEntry.name && (t === undefined || t === 'navigation')) r.unshift(theNavEntry);
        return r;
    },
    clearResourceTimings() {},
    setResourceTimingBufferSize() {},
    get timing() { return theTiming; },
    get navigation() { return theNavigation; },
    toJSON() { return { timeOrigin }; },
});
/* The legacy performance.timing and performance.navigation objects,
 * with their interfaces (pages compare navigation.type against
 * PerformanceNavigation.TYPE_RELOAD). Every time is the time origin. */
class PerformanceTiming {
    constructor() { throw new TypeError('Illegal constructor'); }
}
const timingNames = ['navigationStart', 'unloadEventStart', 'unloadEventEnd', 'redirectStart', 'redirectEnd', 'fetchStart',
    'domainLookupStart', 'domainLookupEnd', 'connectStart', 'connectEnd', 'secureConnectionStart', 'requestStart',
    'responseStart', 'responseEnd', 'domLoading', 'domInteractive', 'domContentLoadedEventStart',
    'domContentLoadedEventEnd', 'domComplete', 'loadEventStart', 'loadEventEnd'];
const timingProps = { toJSON() { const o = {}; for (const k of timingNames) o[k] = this[k]; return o; } };
for (const k of timingNames) Object.defineProperty(timingProps, k, { get() { return Math.round(timeOrigin); }, enumerable: true });
Object.defineProperties(PerformanceTiming.prototype, Object.getOwnPropertyDescriptors(timingProps));
class PerformanceNavigation {
    constructor() { throw new TypeError('Illegal constructor'); }
}
const navConsts = { TYPE_NAVIGATE: 0, TYPE_RELOAD: 1, TYPE_BACK_FORWARD: 2, TYPE_RESERVED: 255 };
for (const k in navConsts) {
    Object.defineProperty(PerformanceNavigation, k, { value: navConsts[k], enumerable: true });
    Object.defineProperty(PerformanceNavigation.prototype, k, { value: navConsts[k], enumerable: true });
}
defProps(PerformanceNavigation.prototype, {
    get type() { return 0; },
    get redirectCount() { return 0; },
    toJSON() { return { type: 0, redirectCount: 0 }; },
});
const theTiming = Object.create(PerformanceTiming.prototype);
const theNavigation = Object.create(PerformanceNavigation.prototype);
tag(Performance);
tag(PerformanceTiming);
tag(PerformanceNavigation);
tag(PerformanceEntry);
tag(PerformanceMark);
tag(PerformanceMeasure);
tag(PerformanceResourceTiming);
tag(PerformanceNavigationTiming);
for (const C of [Performance, PerformanceEntry, PerformanceMark, PerformanceMeasure, PerformanceTiming, PerformanceNavigation,
    PerformanceResourceTiming, PerformanceNavigationTiming])
    defGlobal(C.name, C);
const thePerformance = Object.create(Performance.prototype);
defProps(WP, { get performance() { return thePerformance; } });
class PerformanceObserver {
    constructor(cb) { if (typeof cb !== 'function') throw new TypeError('PerformanceObserver: callback is not a function'); }
    observe() {}
    disconnect() {}
    takeRecords() { return []; }
    static get supportedEntryTypes() { return Object.freeze(['mark', 'measure', 'navigation']); }
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
    /* frame names come before elements' ids and names (the named
     * properties object in prelude_html.js), so those are not counted */
    P.namedOff = true;
    try {
        for (let i = 0; i < names.length; i++) if (names[i] && !(names[i] in G)) def(names[i], i);
    } finally {
        P.namedOff = false;
    }
};

/* ---------------- Storage ---------------- */

/* The areas live with the embedder (N.storage*, AuJsHooks.storage): a
 * Storage object holds only its kind (0 local, 1 session), so every
 * document of an origin reads the same data. The embedder counts the
 * quota and fires the storage event in the other documents
 * (P.storageEvent). */
class Storage {
    constructor() { throw new TypeError('Illegal constructor'); }
}
const storageKind = new WeakMap();
const kindOf = (s) => {
    const k = storageKind.get(s);
    if (k === undefined) throw new TypeError('Illegal invocation');
    return k;
};
defProps(Storage.prototype, {
    get length() { return N.storageLength(kindOf(this)); },
    key(i) {
        if (!arguments.length) throw new TypeError('key: 1 argument required');
        return N.storageKey(kindOf(this), i >>> 0);
    },
    getItem(k) {
        if (!arguments.length) throw new TypeError('getItem: 1 argument required');
        return N.storageGet(kindOf(this), str(k));
    },
    setItem(k, v) {
        if (arguments.length < 2) throw new TypeError('setItem: 2 arguments required');
        const kind = kindOf(this);
        const r = N.storageSet(kind, str(k), str(v));
        if (r === 2) throw domErr('The storage quota has been exceeded', 'QuotaExceededError');
        if (r === 3) throw domErr('Storage is not available for this document', 'SecurityError');
    },
    removeItem(k) {
        if (!arguments.length) throw new TypeError('removeItem: 1 argument required');
        N.storageRemove(kindOf(this), str(k));
    },
    clear() { N.storageClear(kindOf(this)); },
});
tag(Storage);
defGlobal('Storage', Storage);
/* Named properties (the standard's legacy platform object): a string
 * key that is not a property of the prototype chain reads the item; any
 * string key written, defined or deleted acts on the items. */
const storageHandler = {
    get(t, p, r) {
        if (typeof p !== 'string' || p in t) return Reflect.get(t, p, r);
        const v = N.storageGet(kindOf(t), p);
        return v === null ? undefined : v;
    },
    set(t, p, v) { if (typeof p !== 'string') return Reflect.set(t, p, v, t); t.setItem(p, v); return true; },
    defineProperty(t, p, d) {
        if (typeof p !== 'string' || !('value' in d)) return Reflect.defineProperty(t, p, d);
        t.setItem(p, d.value);
        return true;
    },
    has(t, p) { return typeof p === 'string' && N.storageGet(kindOf(t), p) !== null || p in t; },
    deleteProperty(t, p) { if (typeof p !== 'string') return Reflect.deleteProperty(t, p); t.removeItem(p); return true; },
    ownKeys(t) { return N.storageKeys(kindOf(t)).concat(ownKeysAll(t)); },
    getOwnPropertyDescriptor(t, p) {
        if (typeof p !== 'string') return Reflect.getOwnPropertyDescriptor(t, p);
        const v = N.storageGet(kindOf(t), p);
        return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
    },
};
function makeStorage(kind) {
    const s = Object.create(Storage.prototype);
    const p = new Proxy(s, storageHandler);
    storageKind.set(s, kind);
    storageKind.set(p, kind); /* methods see the proxy as this */
    return p;
}
const stores = [null, null];
const storageOf = (kind) => stores[kind] || (stores[kind] = makeStorage(kind));
const storageChecked = (kind) => {
    if (N.storageAccess(kind) !== 0) throw domErr('Storage is not available for this document', 'SecurityError');
    return storageOf(kind);
};
defProps(WP, {
    get localStorage() { return storageChecked(0); },
    get sessionStorage() { return storageChecked(1); },
});
/* Another document of this origin changed the area (a task from the
 * embedder): a = [kind, key, oldValue, newValue, url]. */
P.storageEvent = function (a) {
    fire(G, new StorageEvent('storage', { key: a[1], oldValue: a[2], newValue: a[3], url: a[4], storageArea: storageOf(a[0]) }));
};

/* navigator.storage: the usage and quota of the document's local area.
 * Aura never drops a persistent area, so persist() has nothing to ask. */
class StorageManager { constructor() { throw new TypeError('Illegal constructor'); } }
defProps(StorageManager.prototype, {
    estimate() { const e = N.storageEstimate(0); return Promise.resolve({ usage: e[0], quota: e[1] }); },
    persisted() { return Promise.resolve(N.storageAccess(0) === 0); },
    persist() { return Promise.resolve(N.storageAccess(0) === 0); },
});
tag(StorageManager);
defGlobal('StorageManager', StorageManager);
const theStorageManager = Object.create(StorageManager.prototype);

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

/* ---------------- small APIs pages call without checking ----------------
 * Each does what the standard says a browser without the capability does
 * (docs/web-platform.md, item 5), so the page takes its normal path. */

/* scheduler.postTask and scheduler.yield: tasks in timer order. */
class Scheduler { constructor() { throw new TypeError('Illegal constructor'); } }
defProps(Scheduler.prototype, {
    postTask(cb, opts) {
        if (typeof cb !== 'function') throw new TypeError('postTask: callback is not a function');
        const signal = opts && opts.signal, delay = Math.max(0, Number(opts && opts.delay) || 0);
        return new Promise((resolve, reject) => {
            if (signal && signal.aborted) { reject(signal.reason); return; }
            const id = G.setTimeout(() => {
                if (signal) signal.removeEventListener('abort', onAbort);
                try { resolve(cb()); } catch (e) { reject(e); }
            }, delay);
            const onAbort = () => { G.clearTimeout(id); reject(signal.reason); };
            if (signal) signal.addEventListener('abort', onAbort);
        });
    },
    yield() { return new Promise((resolve) => G.setTimeout(resolve, 0)); },
});
tag(Scheduler);
defGlobal('Scheduler', Scheduler);
const theScheduler = Object.create(Scheduler.prototype);
defProps(WP, { get scheduler() { return theScheduler; } });

/* navigator.permissions: every permission Aura could be asked for is one
 * it does not grant. */
class PermissionStatus extends EventTarget {
    constructor(key, name) {
        if (key !== S.owner) throw new TypeError('Illegal constructor');
        super();
        this._name = name;
    }
    get name() { return this._name; }
    get state() { return 'denied'; }
}
defineHandlers(PermissionStatus.prototype, ['change']);
tag(PermissionStatus);
defGlobal('PermissionStatus', PermissionStatus);
class Permissions { constructor() { throw new TypeError('Illegal constructor'); } }
defProps(Permissions.prototype, {
    query(desc) {
        if (!desc || typeof desc !== 'object' || desc.name === undefined)
            return Promise.reject(new TypeError('permissions.query: a descriptor with a name is required'));
        return Promise.resolve(new PermissionStatus(S.owner, str(desc.name)));
    },
});
tag(Permissions);
defGlobal('Permissions', Permissions);
const thePermissions = Object.create(Permissions.prototype);

/* navigator.locks: the Web Locks API within this document. A shared
 * request waits like an exclusive one, which only orders more strictly. */
class Lock {
    constructor(key, name, mode) {
        if (key !== S.owner) throw new TypeError('Illegal constructor');
        this._name = name;
        this._mode = mode;
    }
    get name() { return this._name; }
    get mode() { return this._mode; }
}
tag(Lock);
defGlobal('Lock', Lock);
const lockTails = new Map(); /* name -> the promise the last request settles */
const lockHeld = new Map();  /* name -> requests holding or waiting */
class LockManager { constructor() { throw new TypeError('Illegal constructor'); } }
defProps(LockManager.prototype, {
    request(name, opts, cb) {
        if (typeof opts === 'function') { cb = opts; opts = {}; }
        opts = opts || {};
        if (typeof cb !== 'function') return Promise.reject(new TypeError('locks.request: callback is not a function'));
        name = str(name);
        if (name.startsWith('-')) return Promise.reject(domErr('Lock names may not start with -', 'NotSupportedError'));
        const mode = opts.mode === 'shared' ? 'shared' : 'exclusive';
        if (opts.signal && opts.signal.aborted) return Promise.reject(opts.signal.reason);
        const busy = (lockHeld.get(name) || 0) > 0;
        if (opts.ifAvailable && busy && !opts.steal) return Promise.resolve().then(() => cb(null));
        const prev = opts.steal ? Promise.resolve() : (lockTails.get(name) || Promise.resolve());
        lockHeld.set(name, (lockHeld.get(name) || 0) + 1);
        const run = prev.then(() => cb(new Lock(S.owner, name, mode)));
        const tail = run.then(() => {}, () => {}).then(() => {
            const n = lockHeld.get(name) - 1;
            if (n > 0) lockHeld.set(name, n); else lockHeld.delete(name);
            if (lockTails.get(name) === tail) lockTails.delete(name);
        });
        lockTails.set(name, tail);
        return run;
    },
    query() {
        const held = [...lockHeld.keys()].map((name) => ({ name, mode: 'exclusive', clientId: '' }));
        return Promise.resolve({ held, pending: [] });
    },
});
tag(LockManager);
defGlobal('LockManager', LockManager);
const theLockManager = Object.create(LockManager.prototype);
/* navigator.clipboard: writing text during a click or key press (copy
 * buttons) through the embedder; reading is refused, as when a browser's
 * user declines. */
class Clipboard extends EventTarget {
    constructor(key) { if (key !== S.owner) throw new TypeError('Illegal constructor'); super(); }
    writeText(text) {
        if (!arguments.length) return Promise.reject(new TypeError('writeText: 1 argument required'));
        text = str(text);
        if (!P.transientActivation()) return Promise.reject(domErr('writeText needs a click or key press', 'NotAllowedError'));
        return N.clipboardWrite(text) ? Promise.resolve() : Promise.reject(domErr('The clipboard is not available', 'NotAllowedError'));
    }
    write(items) {
        if (!P.transientActivation()) return Promise.reject(domErr('write needs a click or key press', 'NotAllowedError'));
        const item = items && items[0];
        if (!item || !item.types || !Array.from(item.types).includes('text/plain'))
            return Promise.reject(domErr('Only text/plain can be written', 'NotAllowedError'));
        return Promise.resolve(item.getType('text/plain')).then((b) => (typeof b === 'string' ? b : b.text())).then((t) => {
            if (!N.clipboardWrite(str(t))) throw domErr('The clipboard is not available', 'NotAllowedError');
        });
    }
    readText() { return Promise.reject(domErr('Reading the clipboard is not allowed', 'NotAllowedError')); }
    read() { return Promise.reject(domErr('Reading the clipboard is not allowed', 'NotAllowedError')); }
}
tag(Clipboard);
defGlobal('Clipboard', Clipboard);
const theClipboard = new Clipboard(S.owner);
class ClipboardItem {
    constructor(items) {
        if (!items || typeof items !== 'object') throw new TypeError('ClipboardItem: an object of types is required');
        this._items = Object.assign({}, items);
    }
    get types() { return Object.freeze(Object.keys(this._items)); }
    getType(t) {
        if (!(t in this._items)) return Promise.reject(domErr('No data of type ' + t, 'NotFoundError'));
        return Promise.resolve(this._items[t]).then((v) => (typeof v === 'string' ? new G.Blob([v], { type: t }) : v));
    }
    static supports(t) { return t === 'text/plain'; }
}
tag(ClipboardItem);
defGlobal('ClipboardItem', ClipboardItem);
class UserActivation { constructor() { throw new TypeError('Illegal constructor'); } }
defProps(UserActivation.prototype, {
    get isActive() { return P.transientActivation(); },
    get hasBeenActive() { return P.hasBeenActive(); },
});
tag(UserActivation);
defGlobal('UserActivation', UserActivation);
const theUserActivation = Object.create(UserActivation.prototype);
defProps(Navigator.prototype, {
    get permissions() { return thePermissions; },
    get locks() { return theLockManager; },
    get clipboard() { return theClipboard; },
    get userActivation() { return theUserActivation; },
});

/* document.startViewTransition: the update runs and the transition is
 * skipped (no animation), as when a browser cannot capture the page. */
class ViewTransition {
    constructor(key, update) {
        if (key !== S.owner) throw new TypeError('Illegal constructor');
        const done = Promise.resolve().then(() => (update ? update() : undefined));
        this._updateDone = done.then(() => undefined);
        this._finished = done.then(() => undefined, () => undefined);
        this._ready = done.then(() => { throw domErr('View transitions are skipped', 'AbortError'); });
        this._ready.catch(() => {});
    }
    get updateCallbackDone() { return this._updateDone; }
    get ready() { return this._ready; }
    get finished() { return this._finished; }
    get types() { return new Set(); }
    skipTransition() {}
}
tag(ViewTransition);
defGlobal('ViewTransition', ViewTransition);
defProps(Document.prototype, {
    startViewTransition(arg) {
        const update = typeof arg === 'function' ? arg : arg && typeof arg.update === 'function' ? arg.update : null;
        return new ViewTransition(S.owner, update);
    },
});

/* Notification: permission is denied, so a notification made anyway
 * fires error. */
class Notification extends EventTarget {
    constructor(title, opts) {
        super();
        if (!arguments.length) throw new TypeError('Notification: 1 argument required');
        this._title = str(title);
        this._body = opts && opts.body !== undefined ? str(opts.body) : '';
        N.queueTask(() => fire(this, new Event('error')));
    }
    static get permission() { return 'denied'; }
    static requestPermission(cb) {
        if (typeof cb === 'function') N.queueTask(() => cb('denied'));
        return Promise.resolve('denied');
    }
    static get maxActions() { return 0; }
    get title() { return this._title; }
    get body() { return this._body; }
    close() {}
}
defineHandlers(Notification.prototype, ['show', 'click', 'close', 'error']);
tag(Notification);
defGlobal('Notification', Notification);

/* BroadcastChannel between the channels of this document. */
const bcChannels = new Map(); /* name -> Set of open channels */
class BroadcastChannel extends EventTarget {
    constructor(name) {
        super();
        if (!arguments.length) throw new TypeError('BroadcastChannel: 1 argument required');
        this._name = str(name);
        this._closed = false;
        if (!bcChannels.has(this._name)) bcChannels.set(this._name, new Set());
        bcChannels.get(this._name).add(this);
    }
    get name() { return this._name; }
    postMessage(data) {
        if (this._closed) throw domErr('The channel is closed', 'InvalidStateError');
        const peers = [...(bcChannels.get(this._name) || [])].filter((c) => c !== this);
        for (const c of peers) {
            const copy = structuredClone(data);
            N.queueTask(() => { if (!c._closed) fire(c, new MessageEvent('message', { data: copy, origin: N.docUrl(2) })); });
        }
    }
    close() {
        this._closed = true;
        const set = bcChannels.get(this._name);
        if (set) { set.delete(this); if (!set.size) bcChannels.delete(this._name); }
    }
}
defineHandlers(BroadcastChannel.prototype, ['message', 'messageerror']);
tag(BroadcastChannel);
defGlobal('BroadcastChannel', BroadcastChannel);
