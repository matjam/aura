/* Aura's JavaScript prelude: the parts of the web platform written in
 * JavaScript over the natives in src/js (events, collections, CSSOM
 * wrappers, window members, fetch, observers, custom elements, the
 * module loader). It runs once per document, before page script, as a
 * function of N (the natives, never reachable from page script) and G
 * (the global object). It returns P, the entry points C calls. */
(function (N, G) {
'use strict';

const P = Object.create(null);
/* null in a worker, which has no document and none of the DOM natives */
const doc = N.document ? N.document() : null;
/* The document's own script is off (js.h, AuJsSettings.scripts_off). */
const scriptsOff = !!(N.scriptsOff && N.scriptsOff());
const S = {
    listeners: Symbol('listeners'), handlers: Symbol('handlers'), ev: Symbol('event'),
    list: Symbol('list'), owner: Symbol('owner'), ce: Symbol('ce'), range: Symbol('range'),
};
const hasOwn = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const defProps = (proto, props) => {
    for (const k of Reflect.ownKeys(props)) {
        const d = Object.getOwnPropertyDescriptor(props, k);
        d.enumerable = false;
        d.configurable = true;
        if ('value' in d) d.writable = true;
        Object.defineProperty(proto, k, d);
    }
};
const defGlobal = (name, value) =>
    Object.defineProperty(G, name, { value, writable: true, configurable: true, enumerable: false });
const tag = (C, name) =>
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name || C.name, configurable: true });
const isNode = doc ? (v) => v !== null && typeof v === 'object' && N.is(v, 0) : () => false;
const isElement = doc ? (v) => v !== null && typeof v === 'object' && N.is(v, 1) : () => false;
const str = (v) => (typeof v === 'string' ? v : String(v));
const domErr = (msg, name) => new DOMException(msg, name);
const timeOriginNow = N.now();
const timeOrigin = Date.now();

/* ---------------- console ---------------- */

function inspect(v, depth, seen) {
    if (typeof v === 'string') return depth ? JSON.stringify(v) : v;
    if (typeof v === 'function') return '[Function ' + (v.name || 'anonymous') + ']';
    if (typeof v === 'symbol' || typeof v === 'bigint') return v.toString() + (typeof v === 'bigint' ? 'n' : '');
    if (v === null || typeof v !== 'object') return String(v);
    if (v instanceof Error) {
        /* top level: with the stack, as browsers' consoles show it */
        const s = v.name + ': ' + v.message;
        return !depth && v.stack ? s + '\n' + String(v.stack).replace(/\n+$/, '') : s;
    }
    if (isNode(v)) {
        if (isElement(v)) {
            let s = '<' + v.localName;
            if (v.id) s += ' id="' + v.id + '"';
            if (v.className && typeof v.className === 'string') s += ' class="' + v.className + '"';
            return s + '>';
        }
        return '#' + v.nodeName.replace(/^#/, '');
    }
    if (seen.has(v)) return '[Circular]';
    if (depth > 2) return Array.isArray(v) ? '[Array]' : '[Object]';
    seen.add(v);
    let out;
    try {
        if (Array.isArray(v)) {
            const parts = v.slice(0, 50).map((x) => inspect(x, depth + 1, seen));
            if (v.length > 50) parts.push('... ' + (v.length - 50) + ' more');
            out = '[' + parts.join(', ') + ']';
        } else if (v instanceof Map) {
            out = 'Map(' + v.size + ') {' + [...v].slice(0, 20).map(([a, b]) =>
                inspect(a, depth + 1, seen) + ' => ' + inspect(b, depth + 1, seen)).join(', ') + '}';
        } else if (v instanceof Set) {
            out = 'Set(' + v.size + ') {' + [...v].slice(0, 20).map((a) => inspect(a, depth + 1, seen)).join(', ') + '}';
        } else if (v instanceof Date) {
            out = isNaN(v) ? 'Invalid Date' : v.toISOString();
        } else if (v instanceof RegExp) {
            out = String(v);
        } else {
            const keys = Object.keys(v);
            const name = v.constructor && v.constructor !== Object && v.constructor.name ? v.constructor.name + ' ' : '';
            out = name + '{' + keys.slice(0, 30).map((k) => k + ': ' + inspect(v[k], depth + 1, seen)).join(', ') +
                (keys.length > 30 ? ', ...' : '') + '}';
        }
    } catch (e) {
        out = '[object]';
    }
    seen.delete(v);
    return out;
}

function format(args) {
    let first = args[0], rest = 1, out = '';
    if (typeof first === 'string' && first.includes('%')) {
        out = first.replace(/%[sdifoOc%]/g, (m) => {
            if (m === '%%') return '%';
            if (rest >= args.length) return m;
            const a = args[rest++];
            switch (m) {
            case '%s': return typeof a === 'string' ? a : inspect(a, 1, new Set());
            case '%d': case '%i': return String(parseInt(a, 10));
            case '%f': return String(Number(a));
            case '%c': return '';
            default: return inspect(a, 1, new Set());
            }
        });
    } else {
        out = args.length ? inspect(first, 0, new Set()) : '';
    }
    for (; rest < args.length; rest++) out += ' ' + inspect(args[rest], 0, new Set());
    return out;
}

let groupIndent = '';
const counts = new Map(), timers = new Map();
const print = (level, s) => N.print(level, groupIndent ? s.replace(/^/gm, groupIndent) : s);
const console = {
    log: (...a) => print(0, format(a)),
    info: (...a) => print(1, format(a)),
    warn: (...a) => print(2, format(a)),
    error: (...a) => print(3, format(a)),
    debug: (...a) => print(4, format(a)),
    trace: (...a) => print(0, format(['Trace:', ...a]) + '\n' + (new Error().stack || '')),
    assert: (cond, ...a) => { if (!cond) print(3, 'Assertion failed' + (a.length ? ': ' + format(a) : '')); },
    dir: (v) => print(0, inspect(v, 0, new Set())),
    dirxml: (...a) => print(0, format(a)),
    table(data, cols) {
        if (data === null || typeof data !== 'object') return print(0, format([data]));
        const rows = Object.keys(data);
        const keys = cols || [...new Set(rows.flatMap((r) =>
            data[r] !== null && typeof data[r] === 'object' ? Object.keys(data[r]) : ['Values']))];
        const lines = ['(index)\t' + keys.join('\t')];
        for (const r of rows) {
            const v = data[r];
            lines.push(r + '\t' + keys.map((k) => k === 'Values' && (v === null || typeof v !== 'object') ?
                inspect(v, 1, new Set()) : (v && k in Object(v) ? inspect(v[k], 1, new Set()) : '')).join('\t'));
        }
        print(0, lines.join('\n'));
    },
    group: (...a) => { if (a.length) print(0, format(a)); groupIndent += '  '; },
    groupCollapsed: (...a) => { if (a.length) print(0, format(a)); groupIndent += '  '; },
    groupEnd: () => { groupIndent = groupIndent.slice(2); },
    count: (l = 'default') => { const n = (counts.get(l) || 0) + 1; counts.set(l, n); print(0, l + ': ' + n); },
    countReset: (l = 'default') => { counts.delete(l); },
    time: (l = 'default') => { timers.set(l, N.now()); },
    timeLog: (l = 'default', ...a) => {
        if (timers.has(l)) print(0, l + ': ' + (N.now() - timers.get(l)).toFixed(3) + ' ms' + (a.length ? ' ' + format(a) : ''));
    },
    timeEnd: (l = 'default') => {
        if (timers.has(l)) { print(0, l + ': ' + (N.now() - timers.get(l)).toFixed(3) + ' ms'); timers.delete(l); }
    },
    clear: () => {},
};
defGlobal('console', console);

/* ---------------- Events ---------------- */

const NONE = 0, CAPTURING = 1, AT_TARGET = 2, BUBBLING = 3;

class Event {
    constructor(type, init) {
        if (arguments.length === 0) throw new TypeError("Failed to construct 'Event': 1 argument required");
        init = init || {};
        this[S.ev] = {
            type: str(type), bubbles: !!init.bubbles, cancelable: !!init.cancelable, composed: !!init.composed,
            target: null, currentTarget: null, phase: NONE, stop: false, stopNow: false, canceled: false,
            passive: false, dispatching: false, initialized: true, trusted: false, path: [],
            timeStamp: N.now() - timeOriginNow, related: null,
        };
    }
    get type() { return this[S.ev].type; }
    get target() { return this[S.ev].target; }
    get srcElement() { return this[S.ev].target; }
    get currentTarget() { return this[S.ev].currentTarget; }
    get eventPhase() { return this[S.ev].phase; }
    get bubbles() { return this[S.ev].bubbles; }
    get cancelable() { return this[S.ev].cancelable; }
    get composed() { return this[S.ev].composed; }
    get defaultPrevented() { return this[S.ev].canceled; }
    get returnValue() { return !this[S.ev].canceled; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this[S.ev].stop; }
    set cancelBubble(v) { if (v) this[S.ev].stop = true; }
    get isTrusted() { return this[S.ev].trusted; }
    get timeStamp() { return this[S.ev].timeStamp; }
    stopPropagation() { this[S.ev].stop = true; }
    stopImmediatePropagation() { const e = this[S.ev]; e.stop = e.stopNow = true; }
    preventDefault() { const e = this[S.ev]; if (e.cancelable && !e.passive) e.canceled = true; }
    composedPath() { return composedPath(this); }
    initEvent(type, bubbles, cancelable) {
        const e = this[S.ev];
        if (e.dispatching) return;
        e.type = str(type); e.bubbles = !!bubbles; e.cancelable = !!cancelable;
        e.stop = e.stopNow = e.canceled = false; e.initialized = true; e.target = null;
    }
}
for (const [k, v] of [['NONE', 0], ['CAPTURING_PHASE', 1], ['AT_TARGET', 2], ['BUBBLING_PHASE', 3]]) {
    Object.defineProperty(Event, k, { value: v, enumerable: true });
    Object.defineProperty(Event.prototype, k, { value: v, enumerable: true });
}
tag(Event);

const initFields = (ev, init, fields) => {
    for (const [k, def] of fields) {
        const v = init && init[k] !== undefined ? init[k] : def;
        Object.defineProperty(ev, k, { value: v, enumerable: true, configurable: true });
    }
};

class CustomEvent extends Event {
    constructor(type, init) {
        super(type, init);
        this[S.ev].detail = init && init.detail !== undefined ? init.detail : null;
    }
    get detail() { return this[S.ev].detail; }
    initCustomEvent(type, bubbles, cancelable, detail) {
        this.initEvent(type, bubbles, cancelable);
        this[S.ev].detail = detail;
    }
}
tag(CustomEvent);

class UIEvent extends Event {
    constructor(type, init) {
        super(type, init);
        this[S.ev].view = init && init.view || null;
        this[S.ev].detail = init && init.detail || 0;
    }
    get view() { return this[S.ev].view; }
    get detail() { return this[S.ev].detail; }
    get which() { return 0; }
    initUIEvent(type, bubbles, cancelable, view, detail) {
        this.initEvent(type, bubbles, cancelable);
        this[S.ev].view = view || null;
        this[S.ev].detail = detail | 0;
    }
}
tag(UIEvent);

const modFields = [['ctrlKey', false], ['shiftKey', false], ['altKey', false], ['metaKey', false]];
class MouseEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        const e = this[S.ev];
        for (const [k, d] of [['screenX', 0], ['screenY', 0], ['clientX', 0], ['clientY', 0], ['button', 0],
            ['buttons', 0], ['movementX', 0], ['movementY', 0]])
            e[k] = init[k] !== undefined ? Number(init[k]) : d;
        for (const [k, d] of modFields) e[k] = init[k] !== undefined ? !!init[k] : d;
        e.related = init.relatedTarget || null;
    }
    get screenX() { return this[S.ev].screenX; }
    get screenY() { return this[S.ev].screenY; }
    get clientX() { return this[S.ev].clientX; }
    get clientY() { return this[S.ev].clientY; }
    get x() { return this[S.ev].clientX; }
    get y() { return this[S.ev].clientY; }
    get pageX() { return this[S.ev].clientX + G.scrollX; }
    get pageY() { return this[S.ev].clientY + G.scrollY; }
    get offsetX() { return this[S.ev].offsetX || 0; }
    get offsetY() { return this[S.ev].offsetY || 0; }
    get button() { return this[S.ev].button; }
    get buttons() { return this[S.ev].buttons; }
    get movementX() { return this[S.ev].movementX; }
    get movementY() { return this[S.ev].movementY; }
    get ctrlKey() { return this[S.ev].ctrlKey; }
    get shiftKey() { return this[S.ev].shiftKey; }
    get altKey() { return this[S.ev].altKey; }
    get metaKey() { return this[S.ev].metaKey; }
    get relatedTarget() { return this[S.ev].related; }
    get which() { return this[S.ev].button + 1; }
    getModifierState(k) {
        const e = this[S.ev];
        return k === 'Control' ? e.ctrlKey : k === 'Shift' ? e.shiftKey : k === 'Alt' ? e.altKey : k === 'Meta' ? e.metaKey : false;
    }
    initMouseEvent(type, bubbles, cancelable, view, detail, sx, sy, cx, cy, ctrl, alt, shift, meta, button, related) {
        this.initUIEvent(type, bubbles, cancelable, view, detail);
        Object.assign(this[S.ev], { screenX: sx | 0, screenY: sy | 0, clientX: cx | 0, clientY: cy | 0, ctrlKey: !!ctrl,
            altKey: !!alt, shiftKey: !!shift, metaKey: !!meta, button: button | 0, related: related || null });
    }
}
tag(MouseEvent);
class WheelEvent extends MouseEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        Object.assign(this[S.ev], { deltaX: +init.deltaX || 0, deltaY: +init.deltaY || 0, deltaZ: +init.deltaZ || 0,
            deltaMode: init.deltaMode | 0 });
    }
    get deltaX() { return this[S.ev].deltaX; }
    get deltaY() { return this[S.ev].deltaY; }
    get deltaZ() { return this[S.ev].deltaZ; }
    get deltaMode() { return this[S.ev].deltaMode; }
}
tag(WheelEvent);
class PointerEvent extends MouseEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        Object.assign(this[S.ev], { pointerId: init.pointerId | 0, pointerType: init.pointerType || 'mouse',
            isPrimary: !!init.isPrimary, width: init.width || 1, height: init.height || 1, pressure: init.pressure || 0 });
    }
    get pointerId() { return this[S.ev].pointerId; }
    get pointerType() { return this[S.ev].pointerType; }
    get isPrimary() { return this[S.ev].isPrimary; }
    get width() { return this[S.ev].width; }
    get height() { return this[S.ev].height; }
    get pressure() { return this[S.ev].pressure; }
}
tag(PointerEvent);

class KeyboardEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        const e = this[S.ev];
        e.key = init.key !== undefined ? str(init.key) : '';
        e.code = init.code !== undefined ? str(init.code) : '';
        e.location = init.location | 0;
        e.repeat = !!init.repeat;
        e.isComposing = !!init.isComposing;
        e.charCode = init.charCode | 0;
        e.keyCode = init.keyCode | 0;
        for (const [k, d] of modFields) e[k] = init[k] !== undefined ? !!init[k] : d;
    }
    get key() { return this[S.ev].key; }
    get code() { return this[S.ev].code; }
    get location() { return this[S.ev].location; }
    get repeat() { return this[S.ev].repeat; }
    get isComposing() { return this[S.ev].isComposing; }
    get ctrlKey() { return this[S.ev].ctrlKey; }
    get shiftKey() { return this[S.ev].shiftKey; }
    get altKey() { return this[S.ev].altKey; }
    get metaKey() { return this[S.ev].metaKey; }
    get charCode() { return this[S.ev].charCode; }
    get keyCode() { return this[S.ev].keyCode; }
    get which() { return this[S.ev].keyCode || this[S.ev].charCode; }
    getModifierState(k) { return MouseEvent.prototype.getModifierState.call(this, k); }
}
Object.assign(KeyboardEvent, { DOM_KEY_LOCATION_STANDARD: 0, DOM_KEY_LOCATION_LEFT: 1, DOM_KEY_LOCATION_RIGHT: 2,
    DOM_KEY_LOCATION_NUMPAD: 3 });
tag(KeyboardEvent);

class FocusEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        this[S.ev].related = init && init.relatedTarget || null;
    }
    get relatedTarget() { return this[S.ev].related; }
}
tag(FocusEvent);

class InputEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        Object.assign(this[S.ev], { data: init.data !== undefined ? init.data : null, inputType: init.inputType || '',
            isComposing: !!init.isComposing });
    }
    get data() { return this[S.ev].data; }
    get inputType() { return this[S.ev].inputType; }
    get isComposing() { return this[S.ev].isComposing; }
}
tag(InputEvent);

class CompositionEvent extends UIEvent {
    constructor(type, init) { super(type, init); this[S.ev].data = init && init.data || ''; }
    get data() { return this[S.ev].data; }
}
tag(CompositionEvent);

const simpleEventClass = (name, fields) => {
    const C = class extends Event {
        constructor(type, init) { super(type, init); initFields(this, init, fields); }
    };
    Object.defineProperty(C, 'name', { value: name });
    tag(C, name);
    return C;
};
const ErrorEvent = simpleEventClass('ErrorEvent', [['message', ''], ['filename', ''], ['lineno', 0], ['colno', 0], ['error', undefined]]);
const PromiseRejectionEvent = simpleEventClass('PromiseRejectionEvent', [['promise', undefined], ['reason', undefined]]);
const PopStateEvent = simpleEventClass('PopStateEvent', [['state', null]]);
const HashChangeEvent = simpleEventClass('HashChangeEvent', [['oldURL', ''], ['newURL', '']]);
const MessageEvent = simpleEventClass('MessageEvent', [['data', null], ['origin', ''], ['lastEventId', ''], ['source', null], ['ports', []]]);
const ProgressEvent = simpleEventClass('ProgressEvent', [['lengthComputable', false], ['loaded', 0], ['total', 0]]);
const StorageEvent = simpleEventClass('StorageEvent', [['key', null], ['oldValue', null], ['newValue', null], ['url', ''], ['storageArea', null]]);
const PageTransitionEvent = simpleEventClass('PageTransitionEvent', [['persisted', false]]);
const SubmitEvent = simpleEventClass('SubmitEvent', [['submitter', null]]);
const TransitionEvent = simpleEventClass('TransitionEvent', [['propertyName', ''], ['elapsedTime', 0], ['pseudoElement', '']]);
const AnimationEvent = simpleEventClass('AnimationEvent', [['animationName', ''], ['elapsedTime', 0], ['pseudoElement', '']]);
const BeforeUnloadEvent = simpleEventClass('BeforeUnloadEvent', []);
const ToggleEvent = simpleEventClass('ToggleEvent', [['oldState', ''], ['newState', '']]);
/* Constructible for pages that make them; Aura fires none of these yet. */
const ClipboardEvent = simpleEventClass('ClipboardEvent', [['clipboardData', null]]);
const FormDataEvent = simpleEventClass('FormDataEvent', [['formData', null]]);
class DragEvent extends MouseEvent {
    constructor(type, init) { super(type, init); this[S.ev].dataTransfer = init && init.dataTransfer || null; }
    get dataTransfer() { return this[S.ev].dataTransfer; }
}
tag(DragEvent);

for (const C of [Event, CustomEvent, UIEvent, MouseEvent, WheelEvent, PointerEvent, KeyboardEvent, FocusEvent,
    InputEvent, CompositionEvent, ErrorEvent, PromiseRejectionEvent, PopStateEvent, HashChangeEvent, MessageEvent,
    ProgressEvent, StorageEvent, PageTransitionEvent, SubmitEvent, TransitionEvent, AnimationEvent,
    BeforeUnloadEvent, ToggleEvent, ClipboardEvent, FormDataEvent, DragEvent])
    defGlobal(C.name, C);

/* ---- EventTarget ---- */

/* C makes EventTarget for a document (wrap.c); a worker's is made here. */
if (!doc) defGlobal('EventTarget', class EventTarget {});
const ET = EventTarget.prototype;
const listenersOf = (t, create) => {
    let l = t[S.listeners];
    if (!l && create) {
        l = new Map();
        Object.defineProperty(t, S.listeners, { value: l, configurable: true });
    }
    return l;
};

function flattenOptions(options) {
    if (typeof options === 'boolean') return { capture: options };
    if (options === null || typeof options !== 'object') return { capture: false };
    return { capture: !!options.capture, once: !!options.once, passive: options.passive, signal: options.signal };
}

defProps(ET, {
    addEventListener(type, callback, options) {
        const target = this == null ? G : this;
        if (callback === null || callback === undefined) return;
        if (typeof callback !== 'function' && typeof callback !== 'object')
            throw new TypeError('addEventListener: the listener is not an object');
        const o = flattenOptions(options);
        if (o.signal !== undefined && !(o.signal instanceof AbortSignal))
            throw new TypeError('addEventListener: signal is not an AbortSignal');
        if (o.signal && o.signal.aborted) return;
        type = str(type);
        const map = listenersOf(target, true);
        let list = map.get(type);
        if (!list) map.set(type, (list = []));
        for (const l of list) if (l.callback === callback && l.capture === o.capture) return;
        const passive = o.passive !== undefined ? !!o.passive :
            (type === 'touchstart' || type === 'touchmove' || type === 'wheel' || type === 'mousewheel') &&
            !!doc && (target === G || target === doc || target === doc.documentElement || target === doc.body);
        const entry = { callback, capture: o.capture, once: !!o.once, passive, removed: false };
        list.push(entry);
        if (o.signal) o.signal.addEventListener('abort', () => target.removeEventListener(type, callback, o.capture));
    },
    removeEventListener(type, callback, options) {
        const target = this == null ? G : this;
        const map = listenersOf(target, false);
        if (!map) return;
        const list = map.get(str(type));
        if (!list) return;
        const capture = flattenOptions(options).capture;
        const i = list.findIndex((l) => l.callback === callback && l.capture === capture && !l.handler);
        if (i >= 0) {
            list[i].removed = true;
            list.splice(i, 1);
        }
    },
    dispatchEvent(event) {
        if (!(event instanceof Event)) throw new TypeError('dispatchEvent: argument is not an Event');
        const e = event[S.ev];
        if (e.dispatching || !e.initialized) throw domErr('The event is already being dispatched', 'InvalidStateError');
        e.trusted = false;
        return dispatch(this == null ? G : this, event);
    },
});

/* ---- dispatch ---- */

const isShadowRoot = (n) => isNode(n) && N.is(n, 4);
function rootOf(n) {
    return isNode(n) ? n.getRootNode() : n;
}
function shadowIncludingAncestor(a, b) {
    /* a is a shadow-including inclusive ancestor of b */
    while (b) {
        if (a === b) return true;
        b = isNode(b) ? (b.parentNode || N.hostOf(b)) : null;
    }
    return false;
}
function retarget(a, b) {
    for (;;) {
        if (!isNode(a)) return a;
        const root = a.getRootNode();
        if (!isShadowRoot(root) || (isNode(b) && shadowIncludingAncestor(root, b))) return a;
        a = N.hostOf(root);
    }
}

/* The parent in the event path: assigned slot, parent, shadow host (when
 * composed or not the target's own root), window for the document. */
function eventParent(n, event, firstTarget) {
    if (n === G) return null;
    if (isNode(n)) {
        if (N.is(n, 2)) return event[S.ev].type === 'load' ? null : (n === doc ? G : null);
        if (isShadowRoot(n)) {
            if (!event[S.ev].composed && rootOf(firstTarget) === n) return null;
            return N.hostOf(n);
        }
        return N.eventParent(n);
    }
    return null;
}

function invoke(item, event, phase) {
    const e = event[S.ev];
    const map = listenersOf(item.node, false);
    if (!map) return;
    const list = map.get(e.type);
    if (!list || !list.length) return;
    e.currentTarget = item.node;
    e.target = item.target;
    e.related = item.related !== undefined ? item.related : e.related;
    for (const l of list.slice()) {
        if (l.removed) continue;
        if (phase === CAPTURING && !l.capture) continue;
        if (phase === BUBBLING && l.capture) continue;
        if (l.once) {
            l.removed = true;
            const i = list.indexOf(l);
            if (i >= 0) list.splice(i, 1);
        }
        e.passive = l.passive;
        try {
            let fn = l.handler ? handlerFunction(item.node, e.type) : l.callback;
            if (typeof fn === 'function') {
                if (l.handler) {
                    let r;
                    if (e.type === 'error' && event instanceof ErrorEvent && item.node === G)
                        r = fn.call(item.node, event.message, event.filename, event.lineno, event.colno, event.error);
                    else r = fn.call(item.node, event);
                    if (e.type === 'error' && r === true && item.node === G) event.preventDefault();
                    else if (e.type === 'beforeunload' && r !== undefined && r !== null) event.preventDefault();
                    else if (r === false && e.type !== 'error') event.preventDefault();
                } else {
                    fn.call(item.node, event);
                }
            } else if (fn && typeof fn.handleEvent === 'function') {
                fn.handleEvent(event);
            }
        } catch (err) {
            reportError(err);
        }
        e.passive = false;
        if (e.stopNow) break;
    }
}

function dispatch(target, event, override) {
    const e = event[S.ev];
    e.dispatching = true;
    const path = [];
    const firstTarget = target;
    const related = e.related;
    let n = target;
    while (n) {
        path.push({ node: n, target: retarget(target, n), related: related ? retarget(related, n) : undefined });
        n = eventParent(n, event, firstTarget);
    }
    if (override) path[0].target = override;
    prepareAttributeHandlers(path, e.type);
    e.path = path;
    e.target = path[0].target;
    /* At target: the first item (whatever target it shows, as for load at
     * window) and shadow hosts the target was retargeted to. */
    for (let i = path.length - 1; i >= 0 && !e.stop; i--) {
        const item = path[i];
        e.phase = i === 0 || item.target === item.node ? AT_TARGET : CAPTURING;
        invoke(item, event, CAPTURING);
    }
    for (let i = 0; i < path.length && !e.stop; i++) {
        const item = path[i];
        const at = i === 0 || item.target === item.node;
        if (!at && !e.bubbles) continue;
        e.phase = at ? AT_TARGET : BUBBLING;
        invoke(item, event, BUBBLING);
    }
    e.phase = NONE;
    e.currentTarget = null;
    e.dispatching = false;
    e.stop = e.stopNow = false;
    e.target = retarget(path[0].target, doc);
    e.path = [];
    return !e.canceled;
}

function composedPath(event) {
    const e = event[S.ev];
    if (!e.path.length) return [];
    const cur = e.currentTarget;
    return e.path.map((p) => p.node).filter((node) => {
        /* hide nodes inside closed shadow trees the current target is not in */
        let r = isNode(node) ? node.getRootNode() : null;
        while (isShadowRoot(r)) {
            if (r.mode === 'closed' && !(isNode(cur) && shadowIncludingAncestor(r, cur))) return false;
            const h = N.hostOf(r);
            r = h ? h.getRootNode() : null;
        }
        return true;
    });
}

/* Fires an event created here (trusted). */
function fire(target, event, override) {
    event[S.ev].trusted = true;
    return dispatch(target, event, override);
}
const fireSimple = (target, type, bubbles, cancelable) =>
    fire(target, new Event(type, { bubbles: !!bubbles, cancelable: !!cancelable }));

/* ---- event handler attributes and properties ---- */

const windowReflectedHandlers = new Set(['load', 'unload', 'beforeunload', 'resize', 'error', 'focus', 'blur',
    'hashchange', 'popstate', 'message', 'storage', 'online', 'offline', 'pagehide', 'pageshow', 'scroll',
    'unhandledrejection', 'rejectionhandled', 'languagechange']);
const handlerTypes = ['abort', 'auxclick', 'beforeinput', 'blur', 'cancel', 'change', 'click', 'close', 'contextmenu',
    'copy', 'cut', 'dblclick', 'drag', 'dragend', 'dragenter', 'dragleave', 'dragover', 'dragstart', 'drop', 'error',
    'focus', 'focusin', 'focusout', 'input', 'invalid', 'keydown', 'keypress', 'keyup', 'load', 'mousedown',
    'mouseenter', 'mouseleave', 'mousemove', 'mouseout', 'mouseover', 'mouseup', 'paste', 'pointerdown', 'pointerup',
    'pointermove', 'pointerover', 'pointerout', 'pointerenter', 'pointerleave', 'pointercancel', 'reset', 'resize',
    'scroll', 'scrollend', 'select', 'submit', 'toggle', 'wheel', 'touchstart', 'touchend', 'touchmove',
    'touchcancel', 'animationstart', 'animationend', 'animationiteration', 'transitionend', 'transitionstart',
    'transitionrun', 'transitioncancel', 'loadstart', 'progress', 'loadend', 'canplay', 'ended', 'play', 'pause',
    'slotchange', 'securitypolicyviolation', 'formdata', 'selectionchange', 'selectstart'];
const windowOnlyTypes = ['beforeunload', 'unload', 'hashchange', 'popstate', 'message', 'messageerror', 'storage',
    'online', 'offline', 'pagehide', 'pageshow', 'unhandledrejection', 'rejectionhandled', 'languagechange',
    'afterprint', 'beforeprint'];

const handlerStore = (t, create) => {
    let h = t[S.handlers];
    if (!h && create) {
        h = new Map();
        Object.defineProperty(t, S.handlers, { value: h, configurable: true });
    }
    return h;
};

function ensureHandlerEntry(t, type, front) {
    const map = listenersOf(t, true);
    let list = map.get(type);
    if (!list) map.set(type, (list = []));
    if (!list.some((l) => l.handler)) {
        const entry = { callback: null, capture: false, once: false, passive: false, removed: false, handler: true };
        if (front) list.unshift(entry); else list.push(entry);
    }
}

/* The attribute node for a handler: the element itself, or the body for
 * window handlers. */
function handlerAttrHost(t, type) {
    if (isElement(t)) return t;
    if (doc && t === G && windowReflectedHandlers.has(type)) return doc.body;
    return null;
}

function handlerFunction(t, type) {
    const store = handlerStore(t, true);
    let h = store.get(type);
    const host = handlerAttrHost(t, type);
    const src = host ? host.getAttribute('on' + type) : null;
    if (h && h.fromProperty) return h.fn;
    if (src === null) return h && h.fn || null;
    if (h && h.src === src) return h.fn;
    let fn = null;
    if (scriptsOff) {
        store.set(type, { src, fn, fromProperty: false });
        return fn;
    }
    try {
        const form = host && host.form ? host.form : {};
        const outer = N.compileHandler(src, 'on' + type);
        fn = outer(host && host.ownerDocument || doc, form || {}, host || t);
    } catch (err) {
        reportError(err);
    }
    store.set(type, { src, fn, fromProperty: false });
    return fn;
}

function defineHandlers(proto, types) {
    const props = {};
    for (const type of types) {
        Object.defineProperty(proto, 'on' + type, {
            configurable: true,
            enumerable: true,
            get() {
                const t = this == null ? G : this;
                return handlerFunction(t, type);
            },
            set(v) {
                const t = this == null ? G : this;
                const store = handlerStore(t, true);
                if (typeof v !== 'function' && (v === null || typeof v !== 'object')) v = null;
                store.set(type, { src: null, fn: v, fromProperty: true });
                if (v) ensureHandlerEntry(t, type, false);
            },
        });
    }
    return props;
}
if (doc) {
    defineHandlers(HTMLElement.prototype, handlerTypes);
    defineHandlers(SVGElement.prototype, handlerTypes);
    defineHandlers(Document.prototype, handlerTypes.concat(['readystatechange', 'visibilitychange', 'DOMContentLoaded']));
    defineHandlers(Window.prototype, handlerTypes.concat(windowOnlyTypes));
}

/* Attribute handlers exist from the moment the attribute does: the entry
 * is put first in the listener list when an event first looks. */
function prepareAttributeHandlers(path, type) {
    for (const item of path) {
        const host = handlerAttrHost(item.node, type);
        if (host && host.hasAttribute('on' + type)) ensureHandlerEntry(item.node, type, true);
    }
}

/* ---- errors ---- */

let inErrorReport = false;
function reportError(err) {
    if (inErrorReport) {
        N.print(3, 'Uncaught ' + describeError(err));
        return;
    }
    /* A report the page cancels still goes to the console, at debug level. */
    if (P.reportException(err)) N.print(3, 'Uncaught ' + describeError(err));
    else N.print(4, 'Uncaught (handled by the page) ' + describeError(err));
}
function describeError(err) {
    try {
        if (err instanceof Error) return String(err) + (err.stack ? '\n' + String(err.stack).replace(/\n+$/, '') : '');
        return inspect(err, 0, new Set());
    } catch (e) {
        return '(error)';
    }
}
P.reportException = function (err) {
    if (inErrorReport) return true;
    inErrorReport = true;
    try {
        /* An error thrown by a script of another origin that was loaded
         * without CORS tells the page nothing but that it happened. */
        let muted = false, file = null, line = 0, col = 0;
        if (err instanceof Error && typeof err.stack === 'string') {
            /* the first frame that names a script (not the prelude or a native) */
            const m = /^\s*at (?:.*? \()?(.*?):(\d+)(?::(\d+))?\)?$/m.exec(err.stack.replace(/^.*(<null>|\(native\)).*$/gm, ''));
            if (m) { file = m[1]; line = +m[2]; col = +m[3] || 0; }
            muted = !!m && N.scriptMuted(m[1]);
        }
        const ev = muted ? new ErrorEvent('error', { cancelable: true, message: 'Script error.', filename: '', lineno: 0, colno: 0, error: null })
            : new ErrorEvent('error', { cancelable: true, message: err instanceof Error ? String(err) : inspect(err, 0, new Set()),
            filename: err && err.fileName || file || N.docUrl(0), lineno: err && err.lineNumber || line, colno: err && err.columnNumber || col,
            error: err });
        return fire(G, ev);
    } catch (e) {
        return true;
    } finally {
        inErrorReport = false;
    }
};
P.unhandledRejection = function (promise, reason) {
    try {
        return fire(G, new PromiseRejectionEvent('unhandledrejection', { cancelable: true, promise, reason }));
    } catch (e) {
        return true;
    }
};
defGlobal('reportError', (e) => { reportError(e); });

/* ---------------- AbortController ---------------- */

class AbortSignal extends EventTarget {
    constructor(key) {
        if (key !== S.owner) throw new TypeError('Illegal constructor');
        super();
        this._aborted = false;
        this._reason = undefined;
    }
    get aborted() { return this._aborted; }
    get reason() { return this._reason; }
    throwIfAborted() { if (this._aborted) throw this._reason; }
    static abort(reason) {
        const s = new AbortSignal(S.owner);
        s._aborted = true;
        s._reason = reason !== undefined ? reason : domErr('signal is aborted without reason', 'AbortError');
        return s;
    }
    static timeout(ms) {
        const s = new AbortSignal(S.owner);
        N.setTimeout(() => signalAbort(s, domErr('signal timed out', 'TimeoutError')), ms);
        return s;
    }
    static any(signals) {
        const s = new AbortSignal(S.owner);
        for (const x of signals) if (x.aborted) { signalAbort(s, x.reason); return s; }
        for (const x of signals) x.addEventListener('abort', () => signalAbort(s, x.reason));
        return s;
    }
}
defineHandlers(AbortSignal.prototype, ['abort']);
function signalAbort(s, reason) {
    if (s._aborted) return;
    s._aborted = true;
    s._reason = reason !== undefined ? reason : domErr('signal is aborted without reason', 'AbortError');
    fireSimple(s, 'abort');
}
class AbortController {
    constructor() { this._signal = new AbortSignal(S.owner); }
    get signal() { return this._signal; }
    abort(reason) { signalAbort(this._signal, reason); }
}
tag(AbortSignal); tag(AbortController);
defGlobal('AbortSignal', AbortSignal);
defGlobal('AbortController', AbortController);

/* ---------------- Intl ---------------- */

/* Intl lives in src/js/intl.js, compiled to its own bytecode. N.intl()
 * reads it the first time script touches Intl or one of the locale
 * methods below; until then the global is a getter that costs nothing. */
let intlLib = null;
const intl = () => intlLib || (intlLib = N.intl()(G));
Object.defineProperty(G, 'Intl', {
    get() { const v = intl().Intl; defGlobal('Intl', v); return v; },
    set(v) { defGlobal('Intl', v); },
    configurable: true, enumerable: false,
});
const thisNumber = (v) => (typeof v === 'number' ? v : Number.prototype.valueOf.call(v));
const thisTime = (v) => Date.prototype.getTime.call(v);
const thisString = (v) => {
    if (v === null || v === undefined) throw new TypeError('String.prototype.localeCompare called on null or undefined');
    return String(v);
};
/* the methods keep their standard lengths (0, and 1 for localeCompare) */
const intlMethods = (proto, props, len) => {
    defProps(proto, props);
    for (const k of Object.keys(props)) Object.defineProperty(proto[k], 'length', { value: len });
};
intlMethods(Number.prototype, {
    toLocaleString(l, o) { return intl().numberToLocale(thisNumber(this), l, o); },
}, 0);
intlMethods(BigInt.prototype, {
    toLocaleString(l, o) { return intl().numberToLocale(BigInt.prototype.valueOf.call(this), l, o); },
}, 0);
intlMethods(Date.prototype, {
    toLocaleString(l, o) { return intl().dateToLocale(thisTime(this), l, o, 'any'); },
    toLocaleDateString(l, o) { return intl().dateToLocale(thisTime(this), l, o, 'date'); },
    toLocaleTimeString(l, o) { return intl().dateToLocale(thisTime(this), l, o, 'time'); },
}, 0);
intlMethods(String.prototype, {
    localeCompare(that, l, o) { return intl().compare(thisString(this), String(that), l, o); },
}, 1);

/* ---------------- Modules read on first use ---------------- */

/* A part of the platform compiled to bytecode of its own (src/js/range.js,
 * cssom.js, canvas.js) and read with N.lazy(index) the first time script
 * touches one of its globals or calls load(). Until then each global is
 * an accessor, one function per name serving as getter and setter, so
 * `name in window`, typeof, instanceof and the window's own property
 * names find it as before. The module is a function of G and the helpers
 * bag (made at that moment, so it may name anything the prelude defines);
 * on loading, the globals become plain data properties, unless the page
 * replaced one. */
const lazyModule = (index, names, helpers) => {
    let lib = null;
    const load = () => {
        if (lib) return lib;
        lib = N.lazy(index)(G, helpers());
        for (const name of names) {
            const d = Object.getOwnPropertyDescriptor(G, name);
            if (d && d.get && d.get === d.set) defGlobal(name, lib[name]);
        }
        return lib;
    };
    for (const name of names) {
        const f = function (v) { if (arguments.length) defGlobal(name, v); else return load()[name]; };
        Object.defineProperty(G, name, { get: f, set: f, configurable: true, enumerable: false });
    }
    return load;
};

/* ---------------- Streams ---------------- */

/* The Streams classes live in src/js/streams.js, compiled to bytecode of
 * their own and read with N.streams() the first time script touches one
 * of these globals or a body's stream (Response.body, Blob.stream());
 * until then each global is a getter that costs nothing. */
const streamGlobals = ['ReadableStream', 'ReadableStreamDefaultReader', 'ReadableStreamBYOBReader',
    'ReadableStreamDefaultController', 'ReadableByteStreamController', 'ReadableStreamBYOBRequest', 'WritableStream',
    'WritableStreamDefaultWriter', 'WritableStreamDefaultController', 'TransformStream',
    'TransformStreamDefaultController', 'ByteLengthQueuingStrategy', 'CountQueuingStrategy', 'TextEncoderStream',
    'TextDecoderStream', 'CompressionStream', 'DecompressionStream'];
let streamsLib = null;
const streams = () => {
    if (streamsLib) return streamsLib;
    streamsLib = N.streams()(G, { N, domErr, reportError, structuredClone, TextDecoder, AbortController, AbortSignal });
    /* the globals become plain data properties, unless the page replaced one */
    for (const name of streamGlobals) {
        const d = Object.getOwnPropertyDescriptor(G, name);
        if (d && d.get && d.get === d.set) defGlobal(name, streamsLib[name]);
    }
    return streamsLib;
};
/* One function per name is both getter and setter (half the closures). */
for (const name of streamGlobals) {
    const f = function (v) { if (arguments.length) defGlobal(name, v); else return streams()[name]; };
    Object.defineProperty(G, name, { get: f, set: f, configurable: true, enumerable: false });
}
