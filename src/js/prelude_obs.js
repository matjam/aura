
/* ---------------- counts shared with C ---------------- */

let observerRegs = 0, ceDefCount = 0, scriptWatch = 0;
const syncCounts = () => N.setCounts(observerRegs, ceDefCount, scriptWatch);

/* ---------------- MutationObserver ---------------- */

const nodeRegs = new WeakMap(); /* node -> [{obs, opts}] */
const liveObservers = new Set();
class MutationRecord {
    constructor() { throw new TypeError('Illegal constructor'); }
}
tag(MutationRecord);
defGlobal('MutationRecord', MutationRecord);
function makeRecord(f) {
    const r = Object.create(MutationRecord.prototype);
    for (const k of ['type', 'target', 'addedNodes', 'removedNodes', 'previousSibling', 'nextSibling', 'attributeName',
        'attributeNamespace', 'oldValue'])
        Object.defineProperty(r, k, { value: f[k] === undefined ? null : f[k], enumerable: true });
    return r;
}
class MutationObserver {
    constructor(cb) {
        if (typeof cb !== 'function') throw new TypeError('MutationObserver: the callback is not a function');
        this._cb = cb;
        this._records = [];
        this._nodes = [];
        this._seq = MutationObserver._seq = (MutationObserver._seq || 0) + 1;
    }
    observe(target, opts = {}) {
        if (!isNode(target)) throw new TypeError('observe: the target is not a Node');
        const o = {
            childList: !!opts.childList, subtree: !!opts.subtree,
            attributes: opts.attributes, characterData: opts.characterData,
            attributeOldValue: !!opts.attributeOldValue, characterDataOldValue: !!opts.characterDataOldValue,
            attributeFilter: opts.attributeFilter !== undefined ? Array.from(opts.attributeFilter, str) : null,
        };
        if (o.attributes === undefined && (o.attributeOldValue || o.attributeFilter)) o.attributes = true;
        if (o.characterData === undefined && o.characterDataOldValue) o.characterData = true;
        o.attributes = !!o.attributes;
        o.characterData = !!o.characterData;
        if (!o.childList && !o.attributes && !o.characterData)
            throw new TypeError('observe: one of childList, attributes or characterData must be true');
        if (o.attributeOldValue && !o.attributes) throw new TypeError('observe: attributeOldValue needs attributes');
        if (o.attributeFilter && !o.attributes) throw new TypeError('observe: attributeFilter needs attributes');
        if (o.characterDataOldValue && !o.characterData) throw new TypeError('observe: characterDataOldValue needs characterData');
        let regs = nodeRegs.get(target);
        if (!regs) nodeRegs.set(target, (regs = []));
        const existing = regs.find((r) => r.obs === this);
        if (existing) existing.opts = o;
        else {
            regs.push({ obs: this, opts: o });
            this._nodes.push(new WeakRef(target));
            N.setObserved(target, true);
            observerRegs++;
            syncCounts();
        }
        liveObservers.add(this);
    }
    disconnect() {
        for (const w of this._nodes) {
            const n = w.deref();
            if (!n) { observerRegs--; continue; }
            const regs = nodeRegs.get(n);
            if (regs) {
                const i = regs.findIndex((r) => r.obs === this);
                if (i >= 0) { regs.splice(i, 1); observerRegs--; }
                if (!regs.length) N.setObserved(n, false);
            }
        }
        this._nodes = [];
        this._records = [];
        liveObservers.delete(this);
        syncCounts();
    }
    takeRecords() { const r = this._records; this._records = []; return r; }
}
tag(MutationObserver);
defGlobal('MutationObserver', MutationObserver);
defGlobal('WebKitMutationObserver', MutationObserver);

/* Raw records from C: [kind, target, added, removed, prev, next, attr, old,
 * observed inclusive ancestors at mutation time, batch]. */
function queueRecord(raw) {
    const [kind, target, added, removed, prev, next, attr, old, observed, batch] = raw;
    const interested = new Map();
    for (const n of observed) {
        const regs = nodeRegs.get(n);
        if (!regs) continue;
        for (const { obs, opts } of regs) {
            if (n !== target && !opts.subtree) continue;
            if (kind === 1 && !opts.childList) continue;
            if (kind === 2) {
                if (!opts.attributes) continue;
                if (opts.attributeFilter && !opts.attributeFilter.includes(attr)) continue;
            }
            if (kind === 3 && !opts.characterData) continue;
            const wantOld = kind === 2 ? opts.attributeOldValue : kind === 3 ? opts.characterDataOldValue : false;
            if (!interested.has(obs) || wantOld) interested.set(obs, wantOld);
        }
    }
    for (const [obs, wantOld] of interested) {
        const recs = obs._records;
        const last = recs[recs.length - 1];
        if (kind === 1) {
            /* Coalesce consecutive inserts (a fragment) and removals of
             * neighbours into one record, as a single DOM call makes. */
            if (last && last.type === 'childList' && last.target === target && last._raw && last._batch === batch) {
                const l = last._raw;
                if (added && l.added.length && !l.removed.length && prev === l.added[l.added.length - 1]) {
                    l.added.push(added); l.next = next; continue;
                }
                if (removed && l.removed.length && !l.added.length && prev === l.prev) {
                    l.removed.push(removed); l.next = next; continue;
                }
            }
            const r = { type: 'childList', target, _batch: batch, _raw: { added: added ? [added] : [], removed: removed ? [removed] : [], prev, next } };
            recs.push(r);
        } else {
            recs.push({ type: kind === 2 ? 'attributes' : 'characterData', target, attributeName: kind === 2 ? attr : null,
                oldValue: wantOld ? old : null });
        }
    }
}
function finishRecords(obs) {
    obs._records = obs._records.map((r) => {
        if (r instanceof MutationRecord) return r;
        if (r._raw) {
            const x = r._raw;
            return makeRecord({ type: r.type, target: r.target, addedNodes: makeList(NodeList, null, -1, null, x.added),
                removedNodes: makeList(NodeList, null, -1, null, x.removed), previousSibling: x.prev, nextSibling: x.next });
        }
        return makeRecord(Object.assign({ addedNodes: makeList(NodeList, null, -1, null, []),
            removedNodes: makeList(NodeList, null, -1, null, []) }, r));
    });
}
P.deliverMutations = function (raws, slotsDirty, notify) {
    for (const raw of raws) queueRecord(raw);
    if (slotsDirty) signalSlots();
    if (!notify) return;
    const list = [...liveObservers].sort((a, b) => a._seq - b._seq);
    for (const obs of list) {
        if (!obs._records.length) continue;
        finishRecords(obs);
        const recs = obs.takeRecords();
        try { obs._cb.call(obs, recs, obs); } catch (e) { reportError(e); }
    }
};
/* takeRecords must also see records still in their raw form. */
const takeRecords0 = MutationObserver.prototype.takeRecords;
MutationObserver.prototype.takeRecords = function () {
    N.flushMutations();
    finishRecords(this);
    return takeRecords0.call(this);
};

/* ---------------- shadow roots and slotchange ---------------- */

const shadowRoots = [];
const slotSnapshots = new WeakMap();
function trackShadowRoot(r) { if (r) shadowRoots.push(new WeakRef(r)); return r; }
function signalSlots() {
    for (let i = shadowRoots.length - 1; i >= 0; i--) {
        const r = shadowRoots[i].deref();
        if (!r) { shadowRoots.splice(i, 1); continue; }
        for (const slot of r.querySelectorAllArray('slot')) {
            const now = N.assigned(slot, false);
            const before = slotSnapshots.get(slot) || [];
            slotSnapshots.set(slot, now);
            if (now.length !== before.length || now.some((n, k) => n !== before[k]))
                N.queueTask(() => fire(slot, new Event('slotchange', { bubbles: true })));
        }
    }
}
const attachShadow0 = EP.attachShadow;
defProps(EP, {
    attachShadow(init) { return trackShadowRoot(attachShadow0.call(this, init)); },
});
/* Declarative shadow roots from the parser: converted after each parse
 * chunk and at the end (P.declarativeShadow is called by the page). */
P.declarativeShadow = function (final) {
    const roots = N.declarativeShadow(doc, !!final);
    if (Array.isArray(roots)) for (const r of roots) trackShadowRoot(r);
};

/* ---------------- custom elements ---------------- */

const ceDefs = new Map();      /* name -> definition */
const ceByCtor = new Map();    /* constructor -> definition */
const ceState = new WeakMap(); /* element -> 'custom' | 'failed' */
const ceWhen = new Map();      /* name -> {promise, resolve} */
const reservedNames = new Set(['annotation-xml', 'color-profile', 'font-face', 'font-face-src', 'font-face-uri',
    'font-face-format', 'font-face-name', 'missing-glyph']);
const validCEName = (n) => /^[a-z][-.0-9_a-z·À-ÖØ-öø-ͽͿ-῿‌‍‿⁀⁰-↏Ⰰ-⿯、-퟿豈-﷏ﷰ-�]*-[-.0-9_a-z·À-ÖØ-öø-ͽͿ-῿‌‍‿⁀⁰-↏Ⰰ-⿯、-퟿豈-﷏ﷰ-�]*$/.test(n) &&
    !reservedNames.has(n);
let ceDefining = false;
function defFor(el) {
    if (!isElement(el) || el.namespaceURI !== HTML_NS) return null;
    const is = el.getAttribute('is');
    const d = ceDefs.get(el.localName);
    if (d && d.localName === el.localName && d.name === el.localName) return d;
    if (is) { const d2 = ceDefs.get(is); if (d2 && d2.localName === el.localName) return d2; }
    return null;
}
function ceCall(el, d, name, args) {
    const fn = d.callbacks[name];
    if (typeof fn !== 'function') return;
    try { fn.apply(el, args); } catch (e) { reportError(e); }
}
function upgrade(el) {
    if (ceState.has(el)) return;
    const d = defFor(el);
    if (!d) return;
    d.stack.push(el);
    let ok = false;
    try {
        const r = new d.ctor();
        if (r !== el) throw new TypeError('The custom element constructor did not return the element');
        ok = true;
    } catch (e) {
        reportError(e);
    } finally {
        d.stack.pop();
    }
    if (!ok) { ceState.set(el, 'failed'); return; }
    ceState.set(el, 'custom');
    N.setDefined(el, true);
    for (const a of d.observed) {
        const v = el.getAttribute(a);
        if (v !== null) ceCall(el, d, 'attributeChangedCallback', [a, null, v, null]);
    }
    if (el.isConnected) ceCall(el, d, 'connectedCallback', []);
}
P.constructElement = function (newTarget, iface) {
    const d = ceByCtor.get(newTarget);
    if (!d) throw new TypeError('Illegal constructor');
    if (d.stack.length) {
        const el = d.stack[d.stack.length - 1];
        if (el === null) throw domErr('This element was already constructed', 'InvalidStateError');
        d.stack[d.stack.length - 1] = null;
        Object.setPrototypeOf(el, newTarget.prototype);
        return el;
    }
    const el = N.createElementRaw(d.localName);
    if (d.localName !== d.name) N.setDefined(el, true);
    Object.setPrototypeOf(el, newTarget.prototype);
    ceState.set(el, 'custom');
    N.setDefined(el, true);
    return el;
};
class CustomElementRegistry {
    constructor() { throw new TypeError('Illegal constructor'); }
    define(name, ctor, opts) {
        name = str(name);
        if (typeof ctor !== 'function' || !ctor.prototype) throw new TypeError('define: the constructor is not a constructor');
        if (!validCEName(name)) throw domErr('"' + name + '" is not a valid custom element name', 'SyntaxError');
        if (ceDefs.has(name)) throw domErr('"' + name + '" has already been defined', 'NotSupportedError');
        if (ceByCtor.has(ctor)) throw domErr('This constructor has already been defined', 'NotSupportedError');
        if (ceDefining) throw domErr('A definition is in progress', 'NotSupportedError');
        let localName = name;
        const ext = opts && opts.extends;
        if (ext !== undefined && ext !== null) {
            if (validCEName(str(ext))) throw domErr('extends must name a built-in element', 'NotSupportedError');
            localName = str(ext);
        }
        ceDefining = true;
        let d;
        try {
            const proto = ctor.prototype;
            if (proto === null || typeof proto !== 'object') throw new TypeError('The prototype is not an object');
            const callbacks = {};
            for (const k of ['connectedCallback', 'disconnectedCallback', 'adoptedCallback', 'attributeChangedCallback']) {
                const v = proto[k];
                if (v !== undefined && typeof v !== 'function') throw new TypeError(k + ' is not a function');
                callbacks[k] = v;
            }
            let observed = [];
            if (callbacks.attributeChangedCallback) {
                const o = ctor.observedAttributes;
                if (o !== undefined) observed = Array.from(o, str);
            }
            d = { name, localName, ctor, callbacks, observed: new Set(observed), stack: [] };
        } finally {
            ceDefining = false;
        }
        ceDefs.set(name, d);
        ceByCtor.set(ctor, d);
        ceDefCount++;
        syncCounts();
        /* upgrade existing elements in tree order */
        const sel = localName === name ? name : localName + '[is="' + name + '"]';
        const roots = [doc];
        for (const w of shadowRoots) { const r = w.deref(); if (r) roots.push(r); }
        for (const r of roots) for (const el of r.querySelectorAllArray(sel)) upgrade(el);
        const w = ceWhen.get(name);
        if (w) { w.resolve(ctor); ceWhen.delete(name); }
    }
    get(name) { const d = ceDefs.get(str(name)); return d ? d.ctor : undefined; }
    getName(ctor) { const d = ceByCtor.get(ctor); return d ? d.name : null; }
    whenDefined(name) {
        name = str(name);
        if (!validCEName(name)) return Promise.reject(domErr('"' + name + '" is not a valid custom element name', 'SyntaxError'));
        const d = ceDefs.get(name);
        if (d) return Promise.resolve(d.ctor);
        let w = ceWhen.get(name);
        if (!w) {
            let resolve;
            const promise = new Promise((r) => { resolve = r; });
            ceWhen.set(name, (w = { promise, resolve }));
        }
        return w.promise;
    }
    upgrade(root) {
        if (!isNode(root)) throw new TypeError('upgrade: the argument is not a Node');
        const list = isElement(root) ? [root].concat(root.querySelectorAllArray('*')) : root.querySelectorAllArray('*');
        for (const el of list) upgrade(el);
    }
}
tag(CustomElementRegistry);
defGlobal('CustomElementRegistry', CustomElementRegistry);
const theRegistry = Object.create(CustomElementRegistry.prototype);
function customElementRegistry() { return theRegistry; }

/* createElement: defined custom elements are constructed synchronously,
 * script-created scripts are watched so they run when connected. */
const scriptCreated = new WeakSet();
const scriptStarted = new WeakSet();
const createElement0 = DP.createElement;
const createElementNS0 = DP.createElementNS;
function afterCreate(el) {
    if (el.localName === 'script' && el.namespaceURI === HTML_NS) {
        scriptCreated.add(el);
        scriptWatch++;
        syncCounts();
    }
    return el;
}
defProps(DP, {
    createElement(name, opts) {
        if (ceDefCount && typeof name === 'string') {
            const lower = this.contentType === 'text/html' ? name.toLowerCase() : name;
            const is = opts && typeof opts === 'object' ? opts.is : typeof opts === 'string' ? opts : undefined;
            const d = is !== undefined ? ceDefs.get(str(is)) : ceDefs.get(lower);
            if (d && d.localName === lower && (is === undefined || d.name !== d.localName)) {
                if (d.name === d.localName) {
                    const el = new d.ctor();
                    if (!isElement(el) || el.localName !== lower) throw new TypeError('The custom element constructor returned the wrong element');
                    return el;
                }
                const el = createElement0.call(this, name, opts);
                upgrade(el);
                return el;
            }
        }
        return afterCreate(createElement0.call(this, name, opts));
    },
    createElementNS(ns, name, opts) { return afterCreate(createElementNS0.call(this, ns, name, opts)); },
});
const cloneNode0 = NP.cloneNode;
defProps(NP, {
    cloneNode(deep) {
        const c = cloneNode0.call(this, deep);
        if (ceDefCount && isElement(c)) {
            const list = [c].concat(deep ? c.querySelectorAllArray('*') : []);
            for (const el of list) upgrade(el);
        }
        return c;
    },
});

P.runReactions = function (arr) {
    for (const [kind, el, attr, old] of arr) {
        if (kind === 5) { scriptConnected(el); continue; }
        const st = ceState.get(el);
        if (kind === 1) {
            if (st === 'custom') { const d = defFor(el) || ceByCtor.get(el.constructor); if (d) ceCall(el, d, 'connectedCallback', []); }
            else if (!st && el.isConnected) upgrade(el);
        } else if (kind === 2) {
            if (st === 'custom') { const d = defFor(el) || ceByCtor.get(el.constructor); if (d) ceCall(el, d, 'disconnectedCallback', []); }
        } else if (kind === 3) {
            if (st !== 'custom') continue;
            const d = defFor(el) || ceByCtor.get(el.constructor);
            if (d && d.observed.has(attr)) ceCall(el, d, 'attributeChangedCallback', [attr, old, el.getAttribute(attr), null]);
        }
    }
};

/* ---------------- scripts inserted by script ---------------- */

const jsMimes = new Set(['application/ecmascript', 'application/javascript', 'application/x-ecmascript',
    'application/x-javascript', 'text/ecmascript', 'text/javascript', 'text/javascript1.0', 'text/javascript1.1',
    'text/javascript1.2', 'text/javascript1.3', 'text/javascript1.4', 'text/javascript1.5', 'text/jscript',
    'text/livescript', 'text/x-ecmascript', 'text/x-javascript']);
/* The script's kind: 'classic', 'module', 'importmap' or null (not run). */
function scriptKind(el) {
    let t = el.getAttribute('type');
    if (t === null || t === '') {
        const lang = el.getAttribute('language');
        if (t === null && lang !== null && lang !== '') return jsMimes.has('text/' + lang.toLowerCase()) ? 'classic' : null;
        return 'classic';
    }
    t = t.trim().toLowerCase();
    if (jsMimes.has(t)) return 'classic';
    if (t === 'module') return 'module';
    if (t === 'importmap') return 'importmap';
    return null;
}
P.scriptKind = scriptKind;
function scriptConnected(el) {
    if (!scriptCreated.has(el) || scriptStarted.has(el) || !el.isConnected) return;
    const src = el.getAttribute('src');
    const text = el.text;
    if (src === null && text === '') return; /* runs once it gets content */
    scriptStarted.add(el);
    scriptCreated.delete(el);
    scriptWatch = Math.max(0, scriptWatch - 1);
    syncCounts();
    const kind = scriptKind(el);
    if (!kind) return;
    if (kind === 'classic' && el.hasAttribute('nomodule')) return;
    if (kind === 'importmap') { P.addImportMap(text); return; }
    if (src !== null) {
        if (src === '') { N.queueTask(() => fireSimple(el, 'error')); return; }
        let url;
        try { url = new URL(src, N.docUrl(1)).href; } catch (e) { N.queueTask(() => fireSimple(el, 'error')); return; }
        if (kind === 'module') {
            runModuleScript(url, null, el).then((ok) => fireSimple(el, ok ? 'load' : 'error'));
            return;
        }
        const co = el.getAttribute('crossorigin');
        P.fetchText(url, 1, (body, msg, finalUrl, crossOrigin) => {
            if (body === null) { fireSimple(el, 'error'); return; }
            if (crossOrigin) N.muteScript(url);
            N.runScript(body, url, el);
            fireSimple(el, 'load');
        }, co === null ? 0 : co.toLowerCase() === 'use-credentials' ? 2 : 1);
        return;
    }
    if (kind === 'module') { runModuleScript(N.docUrl(0), text, el); return; }
    N.runScript(text, N.docUrl(0), el);
}

/* ---------------- modules ---------------- */

function fetchModules(urls) {
    return Promise.all(urls.map((u) => new Promise((resolve) => {
        P.fetchText(u, 2, (body) => {
            if (body === null) { resolve(u); return; }
            N.moduleStore(u, body);
            resolve(null);
        });
    }))).then((failed) => failed.filter(Boolean));
}
/* Loads, links and evaluates a module script. Resolves to true on
 * success. Errors are reported. */
async function runModuleScript(url, src, el) {
    try {
        for (let round = 0; round < 200; round++) {
            const r = N.moduleEval(url, src, src !== null);
            if (Array.isArray(r)) {
                const failed = await fetchModules(r);
                if (failed.length) {
                    N.print(3, 'Failed to load module script: ' + failed.join(', '));
                    if (el) fireSimple(el, 'error');
                    return false;
                }
                continue;
            }
            await r.promise;
            return true;
        }
        N.print(3, 'Module graph too large: ' + url);
        return false;
    } catch (e) {
        reportError(e);
        return false;
    }
}
P.runModule = function (url, src, el, id) {
    runModuleScript(url, src, el).then((ok) => N.moduleFinished(id, ok));
};
P.dynamicImport = function (base, spec, token) {
    (async () => {
        try {
            for (let round = 0; round < 200; round++) {
                const r = N.moduleLoad(base, spec, token);
                if (!Array.isArray(r)) return;
                const failed = await fetchModules(r);
                if (failed.length) throw new TypeError('Failed to fetch dynamically imported module: ' + failed.join(', '));
            }
            throw new TypeError('Module graph too large: ' + spec);
        } catch (e) {
            N.moduleFail(token, e);
        }
    })();
};
P.addImportMap = function (text) {
    let m;
    try { m = JSON.parse(text); } catch (e) { reportError(new SyntaxError('Invalid import map: ' + e.message)); return; }
    if (!m || typeof m !== 'object' || Array.isArray(m)) { reportError(new TypeError('Invalid import map')); return; }
    const pairs = [];
    const imports = m.imports && typeof m.imports === 'object' ? m.imports : {};
    for (const k of Object.keys(imports)) {
        const v = imports[k];
        if (typeof v !== 'string') { N.print(2, 'Import map entry "' + k + '" is not a string; ignored.'); continue; }
        let target;
        try { target = new URL(v, N.docUrl(1)).href; } catch (e) { N.print(2, 'Import map entry "' + k + '" is not a URL; ignored.'); continue; }
        let key = k;
        if (/^(\/|\.\/|\.\.\/)/.test(k)) { try { key = new URL(k, N.docUrl(1)).href; } catch (e) { continue; } }
        if (key.endsWith('/') && !target.endsWith('/')) { N.print(2, 'Import map entry "' + k + '" must map to a URL ending in /.'); continue; }
        pairs.push([key, target]);
    }
    if (m.scopes) N.print(2, 'Import map scopes are not supported; ignored.');
    N.setImportMap(pairs);
};

/* ---------------- IntersectionObserver ---------------- */

const ioList = new Set();
class IntersectionObserverEntry {
    constructor(init) { Object.assign(this, init); }
}
tag(IntersectionObserverEntry);
defGlobal('IntersectionObserverEntry', IntersectionObserverEntry);
const parseMargin = (m) => {
    const parts = str(m || '0px').trim().split(/\s+/).map((p) => {
        const v = parseFloat(p);
        if (!/^-?[\d.]+(px|%)?$/.test(p)) throw domErr('rootMargin must be in pixels or percent', 'SyntaxError');
        return { v, pct: p.endsWith('%') };
    });
    while (parts.length < 4) parts.push(parts[parts.length === 3 ? 1 : parts.length === 2 ? 0 : 0]);
    return parts;
};
class IntersectionObserver {
    constructor(cb, opts = {}) {
        if (typeof cb !== 'function') throw new TypeError('IntersectionObserver: the callback is not a function');
        this._cb = cb;
        this._root = opts.root || null;
        if (this._root !== null && !isElement(this._root) && !N.is(this._root, 2)) throw new TypeError('root must be an Element or Document');
        this._margin = parseMargin(opts.rootMargin);
        let t = opts.threshold === undefined ? [0] : Array.isArray(opts.threshold) ? opts.threshold.slice() : [opts.threshold];
        t = t.map(Number);
        if (t.some((x) => !(x >= 0 && x <= 1))) throw new RangeError('Thresholds must be between 0 and 1');
        this._thresholds = t.sort((a, b) => a - b);
        this._targets = new Map(); /* element -> previous threshold index (-1 initial) */
        this._records = [];
    }
    get root() { return this._root; }
    get rootMargin() { return this._margin.map((m) => m.v + (m.pct ? '%' : 'px')).join(' '); }
    get thresholds() { return Object.freeze(this._thresholds.slice()); }
    observe(el) {
        if (!isElement(el)) throw new TypeError('observe: the target is not an Element');
        if (this._targets.has(el)) return;
        this._targets.set(el, { index: -2, ratio: -1 });
        ioList.add(this);
        scheduleObservation();
    }
    unobserve(el) { this._targets.delete(el); if (!this._targets.size) ioList.delete(this); }
    disconnect() { this._targets.clear(); ioList.delete(this); }
    takeRecords() { const r = this._records; this._records = []; return r; }
}
tag(IntersectionObserver);
defGlobal('IntersectionObserver', IntersectionObserver);
function rectOf(el) {
    const r = N.rect(el);
    if (!r) return null;
    const v = N.viewport();
    return { x: r[0] - v[2], y: r[1] - v[3], w: r[2], h: r[3] };
}
function runIntersections() {
    if (!ioList.size) return;
    const v = N.viewport();
    const now = N.now() - timeOriginNow;
    for (const io of ioList) {
        let root;
        if (io._root && isElement(io._root)) root = rectOf(io._root);
        else root = { x: 0, y: 0, w: v[0], h: v[1] };
        if (root) {
            const m = io._margin;
            const mv = (k, base) => (m[k].pct ? (m[k].v * base) / 100 : m[k].v);
            root = { x: root.x - mv(3, root.w), y: root.y - mv(0, root.h), w: root.w + mv(1, root.w) + mv(3, root.w), h: root.h + mv(0, root.h) + mv(2, root.h) };
        }
        for (const [el, st] of io._targets) {
            const b = el.isConnected ? rectOf(el) : null;
            let isect = null, ratio = 0, intersecting = false;
            if (b && root && (!io._root || (isElement(io._root) && io._root.contains(el)) || N.is(io._root, 2))) {
                const x1 = Math.max(b.x, root.x), y1 = Math.max(b.y, root.y);
                const x2 = Math.min(b.x + b.w, root.x + root.w), y2 = Math.min(b.y + b.h, root.y + root.h);
                if (x2 >= x1 && y2 >= y1) {
                    intersecting = true;
                    isect = { x: x1, y: y1, w: x2 - x1, h: y2 - y1 };
                    const area = b.w * b.h;
                    ratio = area > 0 ? (isect.w * isect.h) / area : 1;
                }
            }
            let index = -1;
            if (intersecting) { index = 0; for (let i = 0; i < io._thresholds.length; i++) if (ratio >= io._thresholds[i]) index = i + 1; }
            if (index === st.index && intersecting === st.intersecting) continue;
            st.index = index;
            st.intersecting = intersecting;
            const br = b ? new DOMRectReadOnly(b.x, b.y, b.w, b.h) : new DOMRectReadOnly();
            io._records.push(new IntersectionObserverEntry({
                time: now, target: el, boundingClientRect: br,
                intersectionRect: isect ? new DOMRectReadOnly(isect.x, isect.y, isect.w, isect.h) : new DOMRectReadOnly(),
                rootBounds: root ? new DOMRectReadOnly(root.x, root.y, root.w, root.h) : null,
                isIntersecting: intersecting, intersectionRatio: ratio, isVisible: false,
            }));
        }
        if (io._records.length) {
            const recs = io.takeRecords();
            try { io._cb.call(io, recs, io); } catch (e) { reportError(e); }
        }
    }
}

/* ---------------- ResizeObserver ---------------- */

const roList = new Set();
class ResizeObserverSize {
    constructor(i, b) { this._i = i; this._b = b; }
    get inlineSize() { return this._i; }
    get blockSize() { return this._b; }
}
class ResizeObserverEntry {
    constructor(target, c, b) {
        this.target = target;
        this.contentRect = new DOMRectReadOnly(c.x, c.y, c.w, c.h);
        this.contentBoxSize = Object.freeze([new ResizeObserverSize(c.w, c.h)]);
        this.borderBoxSize = Object.freeze([new ResizeObserverSize(b.w, b.h)]);
        this.devicePixelContentBoxSize = Object.freeze([new ResizeObserverSize(c.w, c.h)]);
    }
}
for (const C of [ResizeObserverSize, ResizeObserverEntry]) { tag(C); defGlobal(C.name, C); }
class ResizeObserver {
    constructor(cb) {
        if (typeof cb !== 'function') throw new TypeError('ResizeObserver: the callback is not a function');
        this._cb = cb;
        this._targets = new Map();
    }
    observe(el, opts) {
        if (!isElement(el)) throw new TypeError('observe: the target is not an Element');
        this._box = opts && opts.box || 'content-box';
        this._targets.set(el, { w: -1, h: -1 });
        roList.add(this);
        scheduleObservation();
    }
    unobserve(el) { this._targets.delete(el); if (!this._targets.size) roList.delete(this); }
    disconnect() { this._targets.clear(); roList.delete(this); }
}
tag(ResizeObserver);
defGlobal('ResizeObserver', ResizeObserver);
function boxesOf(el) {
    const r = el.isConnected ? N.rect(el) : null;
    if (!r) return null;
    const pl = px(el, 'padding-left') + px(el, 'border-left-width'), pr = px(el, 'padding-right') + px(el, 'border-right-width');
    const pt = px(el, 'padding-top') + px(el, 'border-top-width'), pb = px(el, 'padding-bottom') + px(el, 'border-bottom-width');
    return { b: { w: r[2], h: r[3] }, c: { x: px(el, 'padding-left'), y: px(el, 'padding-top'), w: Math.max(0, r[2] - pl - pr), h: Math.max(0, r[3] - pt - pb) } };
}
function observeResizes() {
    /* Callbacks may resize again: a few rounds, then stop, as browsers
     * report the loop error. */
    for (let round = 0; round < 4 && roList.size; round++) {
        let any = false;
        for (const ro of roList) {
            const entries = [];
            for (const [el, last] of ro._targets) {
                const bx = boxesOf(el) || { b: { w: 0, h: 0 }, c: { x: 0, y: 0, w: 0, h: 0 } };
                const s = ro._box === 'border-box' ? bx.b : bx.c;
                if (s.w === last.w && s.h === last.h) continue;
                last.w = s.w; last.h = s.h;
                entries.push(new ResizeObserverEntry(el, bx.c, bx.b));
            }
            if (entries.length) {
                any = true;
                try { ro._cb.call(ro, entries, ro); } catch (e) { reportError(e); }
            }
        }
        if (!any) return;
        if (round === 3) reportError(new Error('ResizeObserver loop completed with undelivered notifications.'));
    }
}

/* New observations run after the next layout; when nothing else forces
 * one, a frame does. */
let observationScheduled = false;
function scheduleObservation() {
    if (observationScheduled) return;
    observationScheduled = true;
    N.requestAnimationFrame(() => { observationScheduled = false; P.layoutDone(); });
}
P.layoutDone = function () {
    runIntersections();
    observeResizes();
};

/* ---------------- entry points for the embedder ---------------- */

P.fire = function (node, type, bubbles, cancelable) {
    return fire(node || G, new Event(type, { bubbles, cancelable }));
};
const modsOf = (m) => ({ shiftKey: !!(m & 1), ctrlKey: !!(m & 2), altKey: !!(m & 4), metaKey: !!(m & 8) });
let buttonsDown = 0;
P.fireMouse = function (target, type, x, y, button, mods, detail, related) {
    if (type === 'mousedown') buttonsDown |= 1 << button;
    else if (type === 'mouseup') buttonsDown &= ~(1 << button);
    const noBubble = type === 'mouseenter' || type === 'mouseleave';
    const init = Object.assign({ bubbles: !noBubble, cancelable: !noBubble, composed: !noBubble, view: G, detail,
        clientX: x, clientY: y, screenX: x, screenY: y, button, buttons: buttonsDown, relatedTarget: related }, modsOf(mods));
    if (type === 'click' || type === 'auxclick' || type === 'contextmenu') {
        const ev = new PointerEvent(type, Object.assign(init, { pointerId: 1, pointerType: 'mouse', isPrimary: true }));
        ev[S.ev].trusted = true;
        if (type === 'click' && button === 0) {
            if (isElement(target) && isFormControl(target) && isDisabled(target)) return true;
            return activate(target, ev, true);
        }
        return fire(target, ev);
    }
    return fire(target, new MouseEvent(type, init));
};
P.fireKey = function (target, type, key, code, mods, repeat) {
    const kc = key.length === 1 ? key.toUpperCase().charCodeAt(0) :
        { Enter: 13, Tab: 9, Backspace: 8, Escape: 27, ' ': 32, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
            Delete: 46, Home: 36, End: 35, PageUp: 33, PageDown: 34, Shift: 16, Control: 17, Alt: 18, Meta: 91 }[key] || 0;
    /* keypress carries the character code (13 for Enter) in keyCode,
     * charCode and which, as browsers do. */
    const cc = type !== 'keypress' ? 0 : key.length === 1 ? key.charCodeAt(0) : key === 'Enter' ? 13 : 0;
    const init = Object.assign({ bubbles: true, cancelable: true, composed: true, view: G, key, code, repeat,
        keyCode: type === 'keypress' ? (cc || kc) : kc, charCode: cc }, modsOf(mods));
    const t = target || focused || doc.body || doc.documentElement;
    const ev = new KeyboardEvent(type, init);
    ev[S.ev].trusted = true;
    /* Default actions (editing, implicit submission) are the view's. */
    return fire(t, ev);
};
/* Forms: the view moved the focus, edited a field, or submits a form. */
P.focusFromEmbedder = function (el) { setFocus(el || null); };
P.fireInput = function (el, type, inputType, data) {
    const ev = new InputEvent(type, { bubbles: true, cancelable: type === 'beforeinput', composed: true, inputType, data });
    ev[S.ev].trusted = true;
    return fire(el, ev);
};
P.fireSubmit = function (form, submitter) {
    const ev = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: submitter || null });
    ev[S.ev].trusted = true;
    return fire(form, ev);
};
P.patternTest = function (pattern, value) {
    try { return new RegExp('^(?:' + pattern + ')$', 'v').test(value) ? 1 : 0; } catch (e) { return -1; }
};
P.unload = function () {
    const bu = new BeforeUnloadEvent('beforeunload', { cancelable: true });
    fire(G, bu, doc);
    fire(G, new PageTransitionEvent('pagehide', { persisted: false }), doc);
    fire(G, new Event('unload'), doc);
};
/* The document cache (Back and Forward keep the document): whether an
 * unload or beforeunload listener forbids keeping it, and the pagehide
 * and pageshow events of a kept document (persisted true). */
P.hasUnloadHandler = function () {
    const map = listenersOf(G, false);
    for (const type of ['unload', 'beforeunload']) {
        const list = map && map.get(type);
        if (list && list.some((l) => !l.removed)) return true;
    }
    const body = doc.body;
    if (body && (body.hasAttribute('onunload') || body.hasAttribute('onbeforeunload'))) return true;
    return false;
};
P.pageTransition = function (show) {
    fire(G, new PageTransitionEvent(show ? 'pageshow' : 'pagehide', { persisted: true }), doc);
};
P.runJavascriptUrl = function (code) {
    code = str(code).replace(/^\s*javascript:/i, '');
    try { code = decodeURIComponent(code); } catch (e) { /* not percent-encoded: use as is */ }
    N.runScript(code, N.docUrl(0), null);
};

return P;
})
