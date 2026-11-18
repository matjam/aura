
/* CSSOM: style sheets and rules. The build compiles this file to its own
 * bytecode (cssom.bc); the prelude reads it with N.lazy(1) the first time
 * script touches one of the classes below, an element's sheet,
 * styleSheets or adoptedStyleSheets, so a page that never does pays only
 * for the getters. It is a function of G, the global object, and H, the
 * prelude's helpers, and returns the classes and what the prelude's
 * getters call.
 *
 * A CSSStyleSheet stands for the sheet of a style or link element. Its
 * rules are read from the sheet's text (N.sheetText: the text the cascade
 * uses) by a splitter that knows comments, strings and blocks; a change
 * (insertRule, deleteRule, a rule's selector or declarations) writes the
 * sheet's text back with N.sheetSetText, and the document view parses it
 * again. The element's own text does not change, as the standard says.
 *
 * A rule's declarations live in the style attribute of an element that is
 * never inserted, so they are parsed and serialized by the same code as
 * inline styles; a rule that script has not changed keeps its source text.
 *
 * A link element's sheet works the same way: the parsed sheet keeps the
 * text it was fetched as.
 *
 * Not here: constructed sheets (new CSSStyleSheet, adoptedStyleSheets), the
 * sheets of @import rules (CSSImportRule.styleSheet is null), rules nested
 * in a style rule, and the rules of cross-origin sheets (SecurityError,
 * as in other browsers). document.styleSheets is a new list at each read. */
(function (G, H) {
'use strict';

const { N, doc, S, domErr, str, tag } = H;

/* Splits css into its top-level rules: { prelude, block } with block null
 * for a statement at-rule (@import ...;). Invalid pieces are dropped. */
function cssSplitRules(css) {
    const out = [];
    const n = css.length;
    let i = 0;
    const skipString = (q) => { i++; while (i < n && css[i] !== q) { if (css[i] === '\\') i++; if (css[i] === '\n') return; i++; } i++; };
    const skipComment = () => { const e = css.indexOf('*/', i + 2); i = e < 0 ? n : e + 2; };
    while (i < n) {
        const c = css[i];
        if (c === ' ' || c === '\n' || c === '\t' || c === '\r' || c === '\f' || c === ';') { i++; continue; }
        if (c === '/' && css[i + 1] === '*') { skipComment(); continue; }
        if (css.startsWith('<!--', i)) { i += 4; continue; }
        if (css.startsWith('-->', i)) { i += 3; continue; }
        const start = i;
        const at = c === '@';
        let depth = 0, block = null, prelude = null;
        while (i < n) {
            const d = css[i];
            if (d === '"' || d === "'") { skipString(d); continue; }
            if (d === '/' && css[i + 1] === '*') { skipComment(); continue; }
            if (d === '\\') { i += 2; continue; }
            if (d === '(' || d === '[') depth++;
            else if (d === ')' || d === ']') { if (depth) depth--; }
            else if (d === ';' && !depth && at) { prelude = css.slice(start, i); i++; break; }
            else if (d === '}' && !depth) { i++; break; } /* a stray brace ends the bad rule */
            else if (d === '{' && !depth) {
                prelude = css.slice(start, i);
                const b0 = ++i;
                let braces = 1;
                while (i < n && braces) {
                    const e = css[i];
                    if (e === '"' || e === "'") { skipString(e); continue; }
                    if (e === '/' && css[i + 1] === '*') { skipComment(); continue; }
                    if (e === '\\') { i += 2; continue; }
                    if (e === '{') braces++;
                    else if (e === '}') braces--;
                    i++;
                }
                block = css.slice(b0, braces ? n : i - 1);
                break;
            }
            i++;
        }
        if (prelude === null && at && i >= n) prelude = css.slice(start, n); /* "@import url(x)" at the end */
        if (prelude !== null && prelude.trim()) out.push({ prelude: prelude.replace(/\/\*[^]*?\*\//g, ' ').trim(), block });
    }
    return out;
}

const cssRuleTypes = { style: 1, import: 3, media: 4, 'font-face': 5, page: 6, keyframes: 7, keyframe: 8, namespace: 10, supports: 12 };
class CSSRule {
    constructor(key) { if (key !== S.owner) throw new TypeError('Illegal constructor'); }
    get type() { return cssRuleTypes[this._kind] || 0; }
    get parentStyleSheet() { return this._sheet; }
    get parentRule() { return this._parent; }
    get cssText() { return this._changed ? this._build() : this._src; }
    set cssText(v) {}
    _touch() {
        for (let r = this; r; r = r._parent) r._changed = true;
        if (this._sheet) this._sheet._sync();
    }
}
for (const [k, v] of Object.entries({ STYLE_RULE: 1, CHARSET_RULE: 2, IMPORT_RULE: 3, MEDIA_RULE: 4, FONT_FACE_RULE: 5, PAGE_RULE: 6,
    KEYFRAMES_RULE: 7, KEYFRAME_RULE: 8, MARGIN_RULE: 9, NAMESPACE_RULE: 10, COUNTER_STYLE_RULE: 11, SUPPORTS_RULE: 12 })) {
    Object.defineProperty(CSSRule, k, { value: v, enumerable: true });
    Object.defineProperty(CSSRule.prototype, k, { value: v, enumerable: true });
}
/* The declarations of a rule: the style of an element that is never
 * inserted. Changes mark the rule and write the sheet back. */
function ruleStyle(rule) {
    if (rule._style) return rule._style;
    const holder = doc.createElement('span');
    holder.setAttribute('style', rule._block || '');
    const inner = holder.style;
    const mutators = new Set(['setProperty', 'removeProperty']);
    rule._holder = holder;
    rule._style = new Proxy(inner, {
        get(t, p) {
            if (p === 'parentRule') return rule;
            const v = t[p];
            if (typeof v !== 'function') return v;
            return mutators.has(p) ? function (...a) { const r = v.apply(t, a); rule._touch(); return r; } : v.bind(t);
        },
        set(t, p, v) { t[p] = v; rule._touch(); return true; },
        has(t, p) { return p in t; },
    });
    return rule._style;
}
const ruleDecls = (rule) => (rule._holder ? rule._holder.getAttribute('style') || '' : (rule._block || '').trim());

class CSSStyleRule extends CSSRule {
    get selectorText() { return this._prelude; }
    set selectorText(v) {
        v = str(v).trim();
        if (!v || !N.cssSupports('selector(' + v + ')')) return;
        this._prelude = v;
        this._touch();
    }
    get style() { return ruleStyle(this); }
    set style(v) { ruleStyle(this).cssText = str(v); }
    get cssRules() { return makeRuleList([]); }
    _build() { const d = ruleDecls(this); return this._prelude + ' { ' + d + (d ? ' ' : '') + '}'; }
}
class CSSGroupingRule extends CSSRule {
    get cssRules() { return makeRuleList(this._rules()); }
    _rules() {
        if (!this._list) this._list = cssSplitRules(this._block || '').map((p) => makeRule(p, this._sheet, this));
        return this._list;
    }
    insertRule(text, index = 0) {
        const rules = this._rules();
        const r = parseOneRule(text, this._sheet, this);
        index >>>= 0;
        if (index > rules.length) throw domErr('The index is past the end of the rules', 'IndexSizeError');
        rules.splice(index, 0, r);
        this._touch();
        return index;
    }
    deleteRule(index) {
        const rules = this._rules();
        index >>>= 0;
        if (index >= rules.length) throw domErr('There is no rule at that index', 'IndexSizeError');
        const [r] = rules.splice(index, 1);
        r._parent = null; r._sheet = null;
        this._touch();
    }
    _build() { return '@' + this._name + (this._prelude ? ' ' + this._prelude : '') + ' {\n' + this._rules().map((r) => '  ' + r.cssText).join('\n') + '\n}'; }
}
class CSSConditionRule extends CSSGroupingRule {
    get conditionText() { return this._prelude; }
}
class CSSMediaRule extends CSSConditionRule {
    get media() { return makeMediaList(() => this._prelude, (v) => { this._prelude = v; this._touch(); }); }
    set media(v) { this._prelude = str(v); this._touch(); }
}
class CSSSupportsRule extends CSSConditionRule {}
/* "[name] [query]": the name is an identifier followed by a space or the
 * end (style( is a query), and not one of the condition's words. */
class CSSContainerRule extends CSSConditionRule {
    _split() {
        const m = /^(-?[_a-zA-Z\u0080-￿\\][-\w\u0080-￿\\]*)(?=\s|$)\s*([^]*)$/.exec(this._prelude);
        return m && !/^(not|and|or|none)$/i.test(m[1]) ? [m[1], m[2]] : ['', this._prelude];
    }
    get containerName() { return this._split()[0]; }
    get containerQuery() { return this._split()[1]; }
}
class CSSLayerBlockRule extends CSSGroupingRule {
    get name() { return this._prelude; }
}
class CSSImportRule extends CSSRule {
    get href() { const m = /^(?:url\(\s*)?(["']?)([^"')]*)\1/.exec(this._prelude); return m ? m[2] : ''; }
    get media() { const rest = this._prelude.replace(/^(?:url\([^)]*\)|"[^"]*"|'[^']*')\s*/, '').replace(/^(layer(\([^)]*\))?|supports\([^]*?\)\s)\s*/g, ''); return makeMediaList(() => rest, () => {}); }
    get layerName() { const m = /\blayer(?:\(([^)]*)\))?/.exec(this._prelude); return m ? m[1] || '' : null; }
    get styleSheet() { return null; }
}
class CSSFontFaceRule extends CSSRule {
    get style() { return ruleStyle(this); }
    _build() { return '@font-face { ' + ruleDecls(this) + ' }'; }
}
class CSSPageRule extends CSSRule {
    get selectorText() { return this._prelude; }
    get style() { return ruleStyle(this); }
    _build() { return '@page' + (this._prelude ? ' ' + this._prelude : '') + ' { ' + ruleDecls(this) + ' }'; }
}
class CSSNamespaceRule extends CSSRule {
    get prefix() { const m = /^([^\s"'(]+)\s+\S/.exec(this._prelude); return m && !/^url\(/i.test(m[1]) ? m[1] : ''; }
    get namespaceURI() { const m = /(?:url\(\s*)?(["']?)([^"')\s]*)\1\)?\s*$/.exec(this._prelude); return m ? m[2] : ''; }
}
class CSSKeyframeRule extends CSSRule {
    get keyText() { return this._prelude; }
    set keyText(v) {
        v = str(v).trim();
        if (!/^(from|to|[\d.]+%)(\s*,\s*(from|to|[\d.]+%))*$/i.test(v)) throw domErr('The key is not a keyframe selector', 'SyntaxError');
        this._prelude = v;
        this._touch();
    }
    get style() { return ruleStyle(this); }
    _build() { return this._prelude + ' { ' + ruleDecls(this) + ' }'; }
}
const keyOf = (s) => str(s).split(',').map((k) => { k = k.trim().toLowerCase(); return k === 'from' ? '0%' : k === 'to' ? '100%' : k; }).join(',');
class CSSKeyframesRule extends CSSRule {
    get name() { return this._prelude.replace(/^(["'])(.*)\1$/, '$2'); }
    set name(v) { this._prelude = str(v); this._touch(); }
    get cssRules() { return makeRuleList(this._rules()); }
    get length() { return this._rules().length; }
    _rules() {
        if (!this._list) this._list = cssSplitRules(this._block || '').filter((p) => p.block !== null && p.prelude[0] !== '@').map((p) => makeRule(p, this._sheet, this, 'keyframe'));
        return this._list;
    }
    appendRule(text) {
        const ps = cssSplitRules(str(text));
        if (ps.length !== 1 || ps[0].block === null) return;
        this._rules().push(makeRule(ps[0], this._sheet, this, 'keyframe'));
        this._touch();
    }
    findRule(key) { const k = keyOf(key); const rs = this._rules(); for (let i = rs.length - 1; i >= 0; i--) if (keyOf(rs[i]._prelude) === k) return rs[i]; return null; }
    deleteRule(key) { const r = this.findRule(key); if (!r) return; this._rules().splice(this._rules().indexOf(r), 1); this._touch(); }
    _build() { return '@' + this._name + ' ' + this._prelude + ' {\n' + this._rules().map((r) => '  ' + r.cssText).join('\n') + '\n}'; }
}
const cssRuleClasses = { style: CSSStyleRule, media: CSSMediaRule, supports: CSSSupportsRule, container: CSSContainerRule, layer: CSSLayerBlockRule,
    import: CSSImportRule, 'font-face': CSSFontFaceRule, page: CSSPageRule, namespace: CSSNamespaceRule, keyframes: CSSKeyframesRule,
    keyframe: CSSKeyframeRule };
function makeRule(piece, sheet, parent, kind) {
    let name = '', prelude = piece.prelude;
    if (!kind) {
        if (prelude[0] === '@') {
            const m = /^@([-\w]+)\s*([^]*)$/.exec(prelude);
            name = m ? m[1].toLowerCase() : '';
            prelude = m ? m[2].trim() : '';
            kind = name.replace(/^-(webkit|moz|o|ms)-/, '');
            if (kind === 'layer' && piece.block === null) kind = 'layer-statement';
        } else {
            kind = 'style';
        }
    }
    const C = cssRuleClasses[kind] || CSSRule;
    const r = new C(S.owner);
    const src = piece.block === null ? piece.prelude + ';' : piece.prelude + ' {' + piece.block + '}';
    Object.defineProperties(r, {
        _kind: { value: kind }, _name: { value: name }, _prelude: { value: prelude, writable: true }, _block: { value: piece.block },
        _src: { value: src }, _changed: { value: false, writable: true }, _sheet: { value: sheet, writable: true },
        _parent: { value: parent || null, writable: true }, _style: { value: null, writable: true }, _holder: { value: null, writable: true },
        _list: { value: null, writable: true },
    });
    return r;
}
/* One valid rule from text, or a SyntaxError (insertRule). */
function parseOneRule(text, sheet, parent) {
    const ps = cssSplitRules(str(text));
    const bad = () => domErr('The text is not one valid CSS rule', 'SyntaxError');
    if (ps.length !== 1) throw bad();
    const p = ps[0];
    if (p.prelude[0] !== '@') {
        if (p.block === null || !N.cssSupports('selector(' + p.prelude + ')')) throw bad();
    } else if (!/^@[-\w]+/.test(p.prelude)) {
        throw bad();
    }
    return makeRule(p, sheet, parent);
}
function arrayLike(proto, items) {
    const l = Object.create(proto);
    items.forEach((v, i) => Object.defineProperty(l, i, { value: v, enumerable: true }));
    Object.defineProperty(l, 'length', { value: items.length });
    return l;
}
class CSSRuleList {
    constructor() { throw new TypeError('Illegal constructor'); }
    item(i) { i >>>= 0; return i < this.length ? this[i] : null; }
    [Symbol.iterator]() { return Array.prototype[Symbol.iterator].call(this); }
}
const makeRuleList = (rules) => arrayLike(CSSRuleList.prototype, rules);
class MediaList {
    constructor() { throw new TypeError('Illegal constructor'); }
    get mediaText() { return this._get(); }
    set mediaText(v) { this._set(str(v)); }
    _items() { return this._get().split(',').map((s) => s.trim()).filter(Boolean); }
    get length() { return this._items().length; }
    item(i) { const l = this._items(); i >>>= 0; return i < l.length ? l[i] : null; }
    appendMedium(m) { const l = this._items(); m = str(m).trim(); if (m && !l.includes(m)) { l.push(m); this._set(l.join(', ')); } }
    deleteMedium(m) {
        const l = this._items(); const k = l.indexOf(str(m).trim());
        if (k < 0) throw domErr('The medium is not in the list', 'NotFoundError');
        l.splice(k, 1); this._set(l.join(', '));
    }
    toString() { return this._get(); }
}
function makeMediaList(get, set) {
    const l = Object.create(MediaList.prototype);
    Object.defineProperties(l, { _get: { value: get }, _set: { value: set } });
    return l;
}

class StyleSheet {
    constructor(key) { if (key !== S.owner) throw new TypeError('Illegal constructor'); }
    get type() { return 'text/css'; }
    get ownerNode() { return this._el || null; }
    get parentStyleSheet() { return null; }
    get title() { return this._el ? this._el.getAttribute('title') || null : null; }
    get href() {
        if (!this._el || this._el.localName !== 'link') return null;
        const t = N.sheetText(this._el);
        return t ? t[1] : this._el.href;
    }
    get media() {
        if (!this._el) return makeMediaList(() => this._media, (v) => { this._media = v; N.sheetConstruct(this._cid, null, v, null); });
        return makeMediaList(() => this._el.getAttribute('media') || '', (v) => this._el.setAttribute('media', v));
    }
    set media(v) { this.media.mediaText = v; }
    get disabled() { return this._disabled; }
    set disabled(v) {
        v = !!v;
        if (v === this._disabled) return;
        this._disabled = v;
        if (this._el) N.sheetDisable(this._el, v);
        else N.sheetConstruct(this._cid, null, null, v);
    }
}
/* Constructed sheets (new CSSStyleSheet()) have an id the document view
 * knows them by; adoptedStyleSheets names them by id. The view frees a
 * sheet when its object is collected and no tree adopts it. */
let nextSheetId = 0;
const sheetGone = typeof FinalizationRegistry === 'function' ? new FinalizationRegistry((id) => N.sheetDestroy(id)) : null;
const dropImports = (s) => {
    const before = s._rules().length;
    s._list = s._list.filter((r) => r._kind !== 'import');
    if (s._list.length !== before) s._sync();
};
class CSSStyleSheet extends StyleSheet {
    constructor(opts) {
        super(S.owner);
        if (opts === S.owner) return;
        opts = opts == null ? {} : opts;
        const media = opts.media === undefined ? '' : typeof opts.media === 'string' ? opts.media : str(Array.from(opts.media).join(', '));
        const id = ++nextSheetId;
        Object.defineProperties(this, {
            _cid: { value: id }, _ctext: { value: '', writable: true }, _media: { value: media, writable: true },
            _disabled: { value: !!opts.disabled, writable: true }, _list: { value: [], writable: true },
            _parsed: { value: null, writable: true },
        });
        N.sheetConstruct(id, '', media, !!opts.disabled);
        if (sheetGone) sheetGone.register(this, id);
    }
    get ownerRule() { return null; }
    get cssRules() { return makeRuleList(this._rules()); }
    get rules() { return this.cssRules; }
    replaceSync(text) {
        if (!this._cid) throw domErr('Only a constructed style sheet can be replaced', 'NotAllowedError');
        this._ctext = str(text);
        N.sheetConstruct(this._cid, this._ctext, null, null);
        dropImports(this);
    }
    replace(text) {
        try { this.replaceSync(text); } catch (e) { return Promise.reject(e); }
        return Promise.resolve(this);
    }
    /* The sheet's text now: the document view's (which holds what script
     * wrote back), or for a style element in an embedder without sheets
     * its text content. */
    _source() {
        if (this._cid) return this._ctext;
        const t = N.sheetText(this._el);
        if (t) {
            if (this._el.localName === 'link' && !N.sameOrigin(t[1]))
                throw domErr('The rules of a cross-origin style sheet cannot be read', 'SecurityError');
            /* a sheet too large to keep its text */
            if (t[0] === null) throw domErr('The text of this style sheet is not kept', 'NotSupportedError');
            return t[0];
        }
        const own = this._el.localName === 'style' ? this._el.textContent : '';
        if (own !== this._own) { this._own = own; this._written = null; }
        return this._written === null ? own : this._written;
    }
    _rules() {
        const text = this._source();
        if (text !== this._parsed) {
            for (const r of this._list) { r._sheet = null; }
            this._list = cssSplitRules(text).map((p) => makeRule(p, this, null));
            this._parsed = text;
        }
        return this._list;
    }
    _sync() {
        const text = this._list.map((r) => r.cssText).join('\n');
        this._parsed = text;
        if (this._cid) { this._ctext = text; N.sheetConstruct(this._cid, text, null, null); } else if (N.sheetText(this._el)) N.sheetSetText(this._el, text);
        else this._written = text;
    }
    insertRule(text, index = 0) {
        const rules = this._rules();
        const r = parseOneRule(text, this, null);
        if (this._cid && r._kind === 'import') throw domErr('A constructed style sheet cannot have @import rules', 'SyntaxError');
        index >>>= 0;
        if (index > rules.length) throw domErr('The index is past the end of the rules', 'IndexSizeError');
        const isImport = (x) => x._kind === 'import';
        if ((isImport(r) && index > 0 && !isImport(rules[index - 1])) || (!isImport(r) && index < rules.length && isImport(rules[index])))
            throw domErr('An @import rule must come before other rules', 'HierarchyRequestError');
        rules.splice(index, 0, r);
        this._sync();
        return index;
    }
    deleteRule(index) {
        const rules = this._rules();
        index >>>= 0;
        if (index >= rules.length) throw domErr('There is no rule at that index', 'IndexSizeError');
        const [r] = rules.splice(index, 1);
        r._sheet = null;
        this._sync();
    }
    addRule(selector = 'undefined', block = 'undefined', index) {
        this.insertRule(str(selector) + ' { ' + str(block) + ' }', index === undefined ? this._rules().length : index);
        return -1;
    }
    removeRule(index = 0) { this.deleteRule(index); }
}
const sheetCache = new WeakMap();
function sheetFor(el) {
    if (!el.isConnected) return null;
    if (el.localName === 'style') {
        const type = (el.getAttribute('type') || '').trim().toLowerCase();
        if (type && type !== 'text/css') return null;
        if (N.sheetText(el) === false) return null;
    } else {
        if (!el.relList.contains('stylesheet') || !N.sheetText(el)) return null;
    }
    let s = sheetCache.get(el);
    if (!s) {
        s = new CSSStyleSheet(S.owner);
        Object.defineProperties(s, {
            _el: { value: el }, _disabled: { value: false, writable: true }, _list: { value: [], writable: true },
            _parsed: { value: null, writable: true }, _own: { value: null, writable: true }, _written: { value: null, writable: true },
        });
        sheetCache.set(el, s);
    }
    return s;
}
class StyleSheetList {
    constructor() { throw new TypeError('Illegal constructor'); }
    item(i) { i >>>= 0; return i < this.length ? this[i] : null; }
    [Symbol.iterator]() { return Array.prototype[Symbol.iterator].call(this); }
}
/* document.styleSheets (html true: only HTML elements' sheets) and a
 * shadow root's styleSheets */
function styleSheetsOf(scope, html) {
    const out = [];
    for (const el of scope.querySelectorAll('style, link')) {
        if (html && el.namespaceURI !== 'http://www.w3.org/1999/xhtml') continue;
        const s = sheetFor(el);
        if (s) out.push(s);
    }
    return arrayLike(StyleSheetList.prototype, out);
}
/* adoptedStyleSheets: an array (a proxy) whose changes reach the view. */
const adoptedLists = new WeakMap();
const checkAdoptable = (s) => {
    if (!(s instanceof CSSStyleSheet) || !s._cid) throw new TypeError('Only constructed style sheets can be adopted');
    return s;
};
/* CSS.registerProperty: the registrations are @property rules in one
 * constructed sheet of Aura's own, adopted by the document ahead of the
 * page's adopted sheets and never shown to script. */
const registered = new Map();
let regSheet = null;
const adoptIds = (scope, items) =>
    (scope === doc && regSheet ? [regSheet._cid] : []).concat(items.map((s) => s._cid));
function registerProperty(def) {
    if (def === null || typeof def !== 'object') throw new TypeError('registerProperty: a definition is required');
    const name = str(def.name);
    if (!/^--./.test(name)) throw domErr("The name '" + name + "' is not a custom property name", 'SyntaxError');
    if (registered.has(name)) throw domErr("The property '" + name + "' is already registered", 'InvalidModificationError');
    if (def.inherits === undefined) throw new TypeError('registerProperty: inherits is required');
    const syntax = def.syntax === undefined ? '*' : str(def.syntax).trim();
    const initial = def.initialValue === undefined ? null : str(def.initialValue);
    if (syntax !== '*' && initial === null) throw domErr('An initial value is required for this syntax', 'SyntaxError');
    if (initial !== null && /[;{}]/.test(initial)) throw domErr('The initial value is not valid', 'SyntaxError');
    registered.set(name, '@property ' + name + ' { syntax: ' + JSON.stringify(syntax) + '; inherits: ' +
        (def.inherits ? 'true' : 'false') + (initial !== null ? '; initial-value: ' + initial : '') + '; }');
    if (!regSheet) regSheet = new CSSStyleSheet();
    N.sheetConstruct(regSheet._cid, [...registered.values()].join('\n'), null, null);
    N.sheetsAdopt(doc, adoptIds(doc, adoptedOf(doc).arr));
}
function adoptedOf(scope) {
    let l = adoptedLists.get(scope);
    if (!l) {
        const arr = [];
        const sync = () => N.sheetsAdopt(scope, adoptIds(scope, arr));
        l = new Proxy(arr, {
            set(t, k, v) {
                if (typeof k === 'string' && /^\d+$/.test(k)) checkAdoptable(v);
                const ok = Reflect.set(t, k, v);
                sync();
                return ok;
            },
            deleteProperty(t, k) { const ok = Reflect.deleteProperty(t, k); sync(); return ok; },
        });
        l = { arr, proxy: l };
        adoptedLists.set(scope, l);
    }
    return l;
}
const adoptedGet = (scope) => adoptedOf(scope).proxy;
function adoptedSet(scope, v) {
    const items = Array.from(v, checkAdoptable);
    const { arr } = adoptedOf(scope);
    arr.length = 0;
    arr.push(...items);
    N.sheetsAdopt(scope, adoptIds(scope, items));
}
const lib = { sheetFor, styleSheetsOf, adoptedGet, adoptedSet, registerProperty };
for (const C of [CSSRule, CSSStyleRule, CSSGroupingRule, CSSConditionRule, CSSMediaRule, CSSSupportsRule, CSSContainerRule, CSSLayerBlockRule,
    CSSImportRule, CSSFontFaceRule, CSSPageRule, CSSNamespaceRule, CSSKeyframeRule, CSSKeyframesRule, CSSRuleList, MediaList, StyleSheet,
    CSSStyleSheet, StyleSheetList]) {
    tag(C);
    lib[C.name] = C;
}
return lib;
})
