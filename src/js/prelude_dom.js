
/* Interfaces with C-made prototypes get their toStringTag here. */
for (const k of Object.getOwnPropertyNames(G)) {
    if (/^(HTML\w*Element|SVGElement|MathMLElement|Node|Element|Document|HTMLDocument|DocumentFragment|ShadowRoot|DocumentType|CharacterData|Text|CDATASection|Comment|ProcessingInstruction|EventTarget|Window)$/.test(k) &&
        typeof G[k] === 'function') tag(G[k], k);
}

/* ---------------- DOM: collections ---------------- */

const isIndex = (p) => typeof p === 'string' && p.length > 0 && p.length < 11 && /^(0|[1-9][0-9]*)$/.test(p);

/* A list's internal state lives in S.list: {root, kind, arg, v, items}. */
function listItems(t) {
    const s = t[S.list];
    if (s.kind < 0) return s.items;
    const v = N.version(s.root);
    if (v !== s.v || !s.items) {
        s.items = N.collect(s.root, s.kind, s.arg);
        s.v = v;
    }
    return s.items;
}

const listHandler = {
    get(t, p, r) {
        if (isIndex(p)) return listItems(t)[+p];
        if (typeof p === 'string' && t[S.list].named && !(p in t)) {
            const v = t.namedItem(p);
            if (v) return v;
        }
        return Reflect.get(t, p, t);
    },
    set(t, p, v, r) {
        if (isIndex(p)) return true;
        return Reflect.set(t, p, v, t);
    },
    has(t, p) {
        if (isIndex(p)) return +p < listItems(t).length;
        return p in t;
    },
    ownKeys(t) {
        const n = listItems(t).length;
        const keys = [];
        for (let i = 0; i < n; i++) keys.push(String(i));
        return keys.concat(Reflect.ownKeys(t));
    },
    getOwnPropertyDescriptor(t, p) {
        if (isIndex(p)) {
            const items = listItems(t);
            if (+p < items.length) return { value: items[+p], writable: false, enumerable: true, configurable: true };
            return undefined;
        }
        return Reflect.getOwnPropertyDescriptor(t, p);
    },
    defineProperty(t, p, d) {
        if (isIndex(p)) return false;
        return Reflect.defineProperty(t, p, d);
    },
    deleteProperty(t, p) {
        if (isIndex(p)) return false;
        return Reflect.deleteProperty(t, p);
    },
};

function makeList(Cls, root, kind, arg, items) {
    const o = Object.create(Cls.prototype);
    Object.defineProperty(o, S.list, { value: { root, kind, arg, v: -1, items: items || null, named: Cls !== NodeList } });
    return new Proxy(o, listHandler);
}

class NodeList {
    constructor() { throw new TypeError('Illegal constructor'); }
    get length() { return listItems(this).length; }
    item(i) { const v = listItems(this)[i >>> 0]; return v === undefined ? null : v; }
    forEach(fn, thisArg) { const a = listItems(this).slice(); for (let i = 0; i < a.length; i++) fn.call(thisArg, a[i], i, this); }
    entries() { return listItems(this).slice().entries(); }
    keys() { return listItems(this).slice().keys(); }
    values() { return listItems(this).slice().values(); }
    [Symbol.iterator]() { return listItems(this).slice()[Symbol.iterator](); }
}
tag(NodeList);

class HTMLCollection {
    constructor() { throw new TypeError('Illegal constructor'); }
    get length() { return listItems(this).length; }
    item(i) { const v = listItems(this)[i >>> 0]; return v === undefined ? null : v; }
    namedItem(name) {
        name = str(name);
        if (!name) return null;
        for (const e of listItems(this)) if (e.id === name || (e.namespaceURI === HTML_NS && e.getAttribute('name') === name)) return e;
        return null;
    }
    [Symbol.iterator]() { return listItems(this).slice()[Symbol.iterator](); }
}
tag(HTMLCollection);
/* form.elements: a name shared by several controls gives a RadioNodeList. */
class RadioNodeList extends NodeList {
    get value() {
        for (const e of listItems(this)) if (e.localName === 'input' && (e.getAttribute('type') || '').toLowerCase() === 'radio' && e.checked) return e.value;
        return '';
    }
    set value(v) {
        v = str(v);
        for (const e of listItems(this))
            if (e.localName === 'input' && (e.getAttribute('type') || '').toLowerCase() === 'radio' && e.value === v) { e.checked = true; break; }
    }
}
tag(RadioNodeList);
defGlobal('RadioNodeList', RadioNodeList);
function makeRadioNodeList(items) {
    const l = makeList(RadioNodeList, null, -1, null, items);
    l[S.list].named = false;
    return l;
}
/* The controls of a form (or the items of a list) a name matches: one
 * element, a RadioNodeList for several, null for none. */
function namedControls(items, name) {
    name = str(name);
    if (!name) return null;
    const m = items.filter((e) => e.id === name || (e.namespaceURI === HTML_NS && e.getAttribute('name') === name));
    return m.length === 0 ? null : m.length === 1 ? m[0] : makeRadioNodeList(m);
}
class HTMLFormControlsCollection extends HTMLCollection {
    namedItem(name) { return namedControls(listItems(this), name); }
}
class HTMLOptionsCollection extends HTMLCollection {
    get selectedIndex() { return this[S.list].root.selectedIndex; }
    set selectedIndex(v) { this[S.list].root.selectedIndex = v; }
    add(o, before) { this[S.list].root.add(o, before); }
    remove(i) { this[S.list].root.remove(i); }
    set length(n) {
        const sel = this[S.list].root;
        const items = listItems(this);
        for (let i = items.length - 1; i >= n; i--) items[i].remove();
        for (let i = items.length; i < n; i++) sel.appendChild(doc.createElement('option'));
    }
    get length() { return listItems(this).length; }
}
tag(HTMLFormControlsCollection); tag(HTMLOptionsCollection);
for (const C of [NodeList, HTMLCollection, HTMLFormControlsCollection, HTMLOptionsCollection]) defGlobal(C.name, C);
const HTML_NS = 'http://www.w3.org/1999/xhtml';

/* Cached live collections: el.childNodes === el.childNodes. */
const liveCache = new WeakMap();
function cachedList(node, key, make) {
    let m = liveCache.get(node);
    if (!m) liveCache.set(node, (m = new Map()));
    let l = m.get(key);
    if (!l) m.set(key, (l = make()));
    return l;
}

/* ---------------- Node and mixins ---------------- */

const NP = Node.prototype, EP = Element.prototype, DP = Document.prototype, FP = DocumentFragment.prototype;
const CDP = CharacterData.prototype;

for (const [k, v] of [['ELEMENT_NODE', 1], ['ATTRIBUTE_NODE', 2], ['TEXT_NODE', 3], ['CDATA_SECTION_NODE', 4],
    ['ENTITY_REFERENCE_NODE', 5], ['ENTITY_NODE', 6], ['PROCESSING_INSTRUCTION_NODE', 7], ['COMMENT_NODE', 8],
    ['DOCUMENT_NODE', 9], ['DOCUMENT_TYPE_NODE', 10], ['DOCUMENT_FRAGMENT_NODE', 11], ['NOTATION_NODE', 12],
    ['DOCUMENT_POSITION_DISCONNECTED', 1], ['DOCUMENT_POSITION_PRECEDING', 2], ['DOCUMENT_POSITION_FOLLOWING', 4],
    ['DOCUMENT_POSITION_CONTAINS', 8], ['DOCUMENT_POSITION_CONTAINED_BY', 16],
    ['DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC', 32]]) {
    Object.defineProperty(Node, k, { value: v, enumerable: true });
    Object.defineProperty(NP, k, { value: v, enumerable: true });
}

defProps(NP, {
    get childNodes() { return cachedList(this, 'childNodes', () => makeList(NodeList, this, 1)); },
    get baseURI() { return N.docUrl(1); },
    lookupNamespaceURI(prefix) {
        if (prefix === 'xml') return 'http://www.w3.org/XML/1998/namespace';
        if (prefix === 'xmlns') return 'http://www.w3.org/2000/xmlns/';
        if (prefix === null || prefix === '') {
            const e = isElement(this) ? this : this.parentElement || (this.documentElement);
            return e ? e.namespaceURI : null;
        }
        return null;
    },
    lookupPrefix() { return null; },
    isDefaultNamespace(ns) { return this.lookupNamespaceURI(null) === ns; },
});

/* Converts the nodes-or-strings arguments of append/prepend/... into one
 * node. */
function convertNodes(owner, nodes) {
    const d = owner.ownerDocument || owner;
    if (nodes.length === 1) return isNode(nodes[0]) ? nodes[0] : d.createTextNode(str(nodes[0]));
    const f = d.createDocumentFragment();
    for (const n of nodes) f.appendChild(isNode(n) ? n : d.createTextNode(str(n)));
    return f;
}

const parentMixin = {
    get children() { return cachedList(this, 'children', () => makeList(HTMLCollection, this, 0)); },
    append(...nodes) { if (nodes.length) this.appendChild(convertNodes(this, nodes)); },
    prepend(...nodes) { if (nodes.length) this.insertBefore(convertNodes(this, nodes), this.firstChild); },
    replaceChildren(...nodes) {
        const n = nodes.length ? convertNodes(this, nodes) : null;
        while (this.firstChild) this.removeChild(this.firstChild);
        if (n) this.appendChild(n);
    },
    querySelectorAll(sel) { return makeList(NodeList, this, -1, null, this.querySelectorAllArray(sel)); },
    getElementsByTagName(name) {
        name = str(name);
        return cachedList(this, 't:' + name, () => makeList(HTMLCollection, this, 2, name));
    },
    getElementsByTagNameNS(ns, local) {
        return makeList(HTMLCollection, this, -1, null, Array.from(this.getElementsByTagName(local)).filter((e) =>
            ns === '*' || e.namespaceURI === ns));
    },
    getElementsByClassName(names) {
        names = str(names);
        return cachedList(this, 'c:' + names, () => makeList(HTMLCollection, this, 3, names));
    },
};
defProps(EP, parentMixin);
defProps(DP, parentMixin);
defProps(FP, parentMixin);

const childMixin = {
    before(...nodes) {
        const p = this.parentNode;
        if (!p) return;
        let ref = this.previousSibling;
        while (ref && nodes.includes(ref)) ref = ref.previousSibling;
        const n = convertNodes(this, nodes);
        p.insertBefore(n, ref ? ref.nextSibling : p.firstChild);
    },
    after(...nodes) {
        const p = this.parentNode;
        if (!p) return;
        let ref = this.nextSibling;
        while (ref && nodes.includes(ref)) ref = ref.nextSibling;
        p.insertBefore(convertNodes(this, nodes), ref);
    },
    replaceWith(...nodes) {
        const p = this.parentNode;
        if (!p) return;
        let ref = this.nextSibling;
        while (ref && nodes.includes(ref)) ref = ref.nextSibling;
        const n = convertNodes(this, nodes);
        if (this.parentNode === p) p.replaceChild(n, this); else p.insertBefore(n, ref);
    },
    remove() { if (this.parentNode) this.parentNode.removeChild(this); },
};
defProps(EP, childMixin);
defProps(CDP, childMixin);
defProps(DocumentType.prototype, childMixin);

/* ---------------- CharacterData, Text ---------------- */

const checkOffset = (len, off) => {
    if (off > len) throw domErr('The offset is out of range', 'IndexSizeError');
};
const cdData = (t) => {
    if (!N.is(t, 6)) throw new TypeError('Illegal invocation');
    return t.data;
};
defProps(CDP, {
    get length() { return cdData(this).length; },
    substringData(off, count) { const d = cdData(this); off >>>= 0; checkOffset(d.length, off); return d.substr(off, count >>> 0); },
    appendData(s) { replaceData(this, cdData(this).length, 0, str(s)); },
    insertData(off, s) { replaceData(this, off >>> 0, 0, str(s)); },
    deleteData(off, count) { replaceData(this, off >>> 0, count >>> 0, ''); },
    replaceData(off, count, s) { replaceData(this, off >>> 0, count >>> 0, str(s)); },
});
/* The standard's "replace data": live ranges in the node move with the
 * edit (N.rangeText), then the data is set. */
function replaceData(node, off, count, s) {
    const d = cdData(node);
    checkOffset(d.length, off);
    if (off + count > d.length) count = d.length - off;
    N.rangeText(node, off, count, s.length);
    try { node.data = d.slice(0, off) + s + d.slice(off + count); } finally { N.rangeText(); }
}
defProps(Text.prototype, {
    splitText(off) {
        if (!N.is(this, 5)) throw new TypeError('Illegal invocation');
        const d = this.data;
        off >>>= 0;
        checkOffset(d.length, off);
        const t = (this.ownerDocument || doc).createTextNode(d.slice(off));
        if (this.parentNode) {
            this.parentNode.insertBefore(t, this.nextSibling);
            N.rangeSplit(this, off, t);
        }
        replaceData(this, off, d.length - off, '');
        return t;
    },
    get wholeText() {
        let a = this;
        while (a.previousSibling && a.previousSibling.nodeType === 3) a = a.previousSibling;
        let s = '';
        for (let b = a; b && b.nodeType === 3; b = b.nextSibling) s += b.data;
        return s;
    },
});

/* ---------------- Attr, NamedNodeMap ---------------- */

class Attr {
    constructor() { throw new TypeError('Illegal constructor'); }
    get name() { return this._name; }
    get localName() { const i = this._name.indexOf(':'); return this._ns && i >= 0 ? this._name.slice(i + 1) : this._name; }
    get prefix() { const i = this._name.indexOf(':'); return this._ns && i >= 0 ? this._name.slice(0, i) : null; }
    get namespaceURI() { return this._ns || null; }
    get ownerElement() { return this._el; }
    get ownerDocument() { return this._el ? this._el.ownerDocument : doc; }
    get specified() { return true; }
    get value() { return this._el ? (this._el.getAttribute(this._name) ?? this._value) : this._value; }
    set value(v) { v = str(v); this._value = v; if (this._el) this._el.setAttribute(this._name, v); }
    get nodeValue() { return this.value; }
    set nodeValue(v) { this.value = v; }
    get textContent() { return this.value; }
    set textContent(v) { this.value = v; }
    get nodeName() { return this._name; }
    get nodeType() { return 2; }
}
tag(Attr);
defGlobal('Attr', Attr);
const attrCache = new WeakMap();
function attrNode(el, name, value, ns) {
    let m = el && attrCache.get(el);
    if (el && !m) attrCache.set(el, (m = new Map()));
    let a = m && m.get(name);
    if (!a) {
        a = Object.create(Attr.prototype);
        a._el = el; a._name = name; a._value = value; a._ns = ns === 1 ? 'http://www.w3.org/1999/xlink' :
            ns === 2 ? 'http://www.w3.org/XML/1998/namespace' : ns === 3 ? 'http://www.w3.org/2000/xmlns/' : null;
        if (m) m.set(name, a);
    }
    return a;
}
function attrAt(el, i) {
    const r = N.attrAt(el, i);
    return r ? attrNode(el, r[0], r[1], r[2]) : null;
}

class NamedNodeMap {
    constructor() { throw new TypeError('Illegal constructor'); }
    get length() { return N.attrCount(this._el); }
    item(i) { return attrAt(this._el, i >>> 0); }
    getNamedItem(name) {
        name = str(name);
        if (this._el.namespaceURI === HTML_NS) name = name.toLowerCase();
        return this._el.hasAttribute(name) ? attrNode(this._el, name, '', 0) : null;
    }
    getNamedItemNS(ns, local) { const v = this._el.getAttributeNS(ns, local); return v === null ? null : this.getNamedItem(local); }
    setNamedItem(attr) { return this._el.setAttributeNode(attr); }
    setNamedItemNS(attr) { return this._el.setAttributeNode(attr); }
    removeNamedItem(name) {
        const a = this.getNamedItem(name);
        if (!a) throw domErr('No such attribute', 'NotFoundError');
        return this._el.removeAttributeNode(a);
    }
    removeNamedItemNS(ns, local) { return this.removeNamedItem(local); }
    *[Symbol.iterator]() { for (let i = 0; i < this.length; i++) yield this.item(i); }
}
tag(NamedNodeMap);
defGlobal('NamedNodeMap', NamedNodeMap);
const nnmHandler = {
    get(t, p) {
        if (isIndex(p)) return t.item(+p) || undefined;
        if (typeof p === 'string' && !(p in t)) { const a = t.getNamedItem(p); if (a) return a; }
        return Reflect.get(t, p, t);
    },
    has(t, p) { return isIndex(p) ? +p < t.length : p in t || (typeof p === 'string' && t._el.hasAttribute(p)); },
    ownKeys(t) {
        const k = [];
        for (let i = 0; i < t.length; i++) k.push(String(i));
        return k.concat(Reflect.ownKeys(t));
    },
    getOwnPropertyDescriptor(t, p) {
        if (isIndex(p)) { const a = t.item(+p); return a ? { value: a, enumerable: true, configurable: true, writable: false } : undefined; }
        return Reflect.getOwnPropertyDescriptor(t, p);
    },
};

/* insertAdjacentElement and insertAdjacentText (insertAdjacentHTML is
 * native). */
function insertAdjacent(el, where, node) {
    switch (str(where).toLowerCase()) {
    case 'beforebegin': return el.parentNode ? el.parentNode.insertBefore(node, el) : null;
    case 'afterbegin': return el.insertBefore(node, el.firstChild);
    case 'beforeend': return el.appendChild(node);
    case 'afterend': return el.parentNode ? el.parentNode.insertBefore(node, el.nextSibling) : null;
    default: throw domErr('insertAdjacent: bad position ' + where, 'SyntaxError');
    }
}
defProps(EP, {
    insertAdjacentElement(where, node) {
        if (!isElement(node)) throw new TypeError('insertAdjacentElement: not an Element');
        return insertAdjacent(this, where, node);
    },
    insertAdjacentText(where, data) { insertAdjacent(this, where, (this.ownerDocument || doc).createTextNode(str(data))); },
});

defProps(EP, {
    get attributes() {
        return cachedList(this, 'attributes', () => {
            const m = Object.create(NamedNodeMap.prototype);
            Object.defineProperty(m, '_el', { value: this });
            return new Proxy(m, nnmHandler);
        });
    },
    getAttributeNode(name) { return this.attributes.getNamedItem(name); },
    getAttributeNodeNS(ns, local) { return this.attributes.getNamedItemNS(ns, local); },
    setAttributeNode(attr) {
        if (!(attr instanceof Attr)) throw new TypeError('setAttributeNode: not an Attr');
        if (attr._el && attr._el !== this) throw domErr('The attribute is in use', 'InUseAttributeError');
        const old = this.hasAttribute(attr._name) ? attrNode(this, attr._name, '', 0) : null;
        const value = attr.value;
        if (old && old !== attr) { old._value = this.getAttribute(attr._name); old._el = null; }
        let m = attrCache.get(this);
        if (!m) attrCache.set(this, (m = new Map()));
        attr._el = this;
        m.set(attr._name, attr);
        this.setAttribute(attr._name, value);
        return old === attr ? null : old;
    },
    setAttributeNodeNS(attr) { return this.setAttributeNode(attr); },
    removeAttributeNode(attr) {
        if (!(attr instanceof Attr) || attr._el !== this) throw domErr('The attribute is not on this element', 'NotFoundError');
        attr._value = this.getAttribute(attr._name);
        this.removeAttribute(attr._name);
        const m = attrCache.get(this);
        if (m) m.delete(attr._name);
        attr._el = null;
        return attr;
    },
});
defProps(DP, {
    createAttribute(name) {
        name = str(name);
        if (!/^[^\s"'>/=\x00-\x1f\x7f]+$/.test(name)) throw domErr('Invalid attribute name', 'InvalidCharacterError');
        return attrNode(null, name.toLowerCase(), '', 0);
    },
    createAttributeNS(ns, name) { const a = attrNode(null, str(name), '', 0); a._ns = ns; return a; },
});

/* ---------------- DOMTokenList ---------------- */

const splitTokens = (s) => { const t = str(s).split(/[ \t\n\f\r]+/).filter(Boolean); return [...new Set(t)]; };
const checkToken = (t) => {
    t = str(t);
    if (!t) throw domErr('The token must not be empty', 'SyntaxError');
    if (/[ \t\n\f\r]/.test(t)) throw domErr('The token must not contain whitespace', 'InvalidCharacterError');
    return t;
};
class DOMTokenList {
    constructor() { throw new TypeError('Illegal constructor'); }
    _get() { return splitTokens(this._el.getAttribute(this._attr) || ''); }
    _set(list) { this._el.setAttribute(this._attr, list.join(' ')); }
    get length() { return this._get().length; }
    get value() { return this._el.getAttribute(this._attr) || ''; }
    set value(v) { this._el.setAttribute(this._attr, str(v)); }
    toString() { return this.value; }
    item(i) { const v = this._get()[i >>> 0]; return v === undefined ? null : v; }
    contains(t) { return this._get().includes(str(t)); }
    add(...tokens) {
        tokens = tokens.map(checkToken);
        const l = this._get();
        for (const t of tokens) if (!l.includes(t)) l.push(t);
        this._set(l);
    }
    remove(...tokens) {
        tokens = tokens.map(checkToken);
        if (!this._el.hasAttribute(this._attr)) return;
        this._set(this._get().filter((x) => !tokens.includes(x)));
    }
    toggle(t, force) {
        t = checkToken(t);
        const l = this._get();
        const has = l.includes(t);
        if (has && force !== true) { this._set(l.filter((x) => x !== t)); return false; }
        if (!has && force !== false) { l.push(t); this._set(l); return true; }
        return has;
    }
    replace(a, b) {
        a = checkToken(a); b = checkToken(b);
        const l = this._get();
        const i = l.indexOf(a);
        if (i < 0) return false;
        const out = [];
        for (const x of l) { const y = x === a ? b : x; if (!out.includes(y)) out.push(y); }
        this._set(out);
        return true;
    }
    supports(t) {
        const ok = this._supported;
        if (!ok) throw new TypeError("This DOMTokenList has no supported tokens");
        return ok.includes(str(t).toLowerCase());
    }
    forEach(fn, thisArg) { this._get().forEach((v, i) => fn.call(thisArg, v, i, this)); }
    entries() { return this._get().entries(); }
    keys() { return this._get().keys(); }
    values() { return this._get().values(); }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
}
tag(DOMTokenList);
defGlobal('DOMTokenList', DOMTokenList);
const tokenHandler = {
    get(t, p) { return isIndex(p) ? t.item(+p) ?? undefined : Reflect.get(t, p, t); },
    has(t, p) { return isIndex(p) ? +p < t.length : p in t; },
    ownKeys(t) { return t._get().map((_, i) => String(i)).concat(Reflect.ownKeys(t)); },
    getOwnPropertyDescriptor(t, p) {
        if (isIndex(p)) { const v = t.item(+p); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: false }; }
        return Reflect.getOwnPropertyDescriptor(t, p);
    },
};
function tokenList(el, attr, supported) {
    return cachedList(el, 'tl:' + attr, () => {
        const l = Object.create(DOMTokenList.prototype);
        Object.defineProperties(l, { _el: { value: el }, _attr: { value: attr }, _supported: { value: supported } });
        return new Proxy(l, tokenHandler);
    });
}
defProps(EP, {
    get classList() { return tokenList(this, 'class'); },
    set classList(v) { this.setAttribute('class', str(v)); },
});

/* ---------------- dataset ---------------- */

const dataName = (p) => 'data-' + p.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
const camel = (s) => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
class DOMStringMap { constructor() { throw new TypeError('Illegal constructor'); } }
tag(DOMStringMap);
defGlobal('DOMStringMap', DOMStringMap);
const datasetHandler = {
    get(t, p) {
        if (typeof p !== 'string') return Reflect.get(t, p);
        const v = t[S.owner].getAttribute(dataName(p));
        return v === null ? undefined : v;
    },
    set(t, p, v) {
        if (typeof p !== 'string') return Reflect.set(t, p, v);
        if (/-[a-z]/.test(p)) throw domErr('Invalid data attribute name', 'SyntaxError');
        t[S.owner].setAttribute(dataName(p), str(v));
        return true;
    },
    deleteProperty(t, p) { if (typeof p === 'string') t[S.owner].removeAttribute(dataName(p)); return true; },
    has(t, p) { return typeof p === 'string' && t[S.owner].hasAttribute(dataName(p)); },
    ownKeys(t) {
        return t[S.owner].getAttributeNames().filter((n) => n.startsWith('data-')).map((n) => camel(n.slice(5)))
            .concat(Reflect.ownKeys(t));
    },
    getOwnPropertyDescriptor(t, p) {
        if (typeof p !== 'string') return undefined;
        const v = t[S.owner].getAttribute(dataName(p));
        return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
    },
};
const datasetOf = (el) => cachedList(el, 'dataset', () => {
    const o = Object.create(DOMStringMap.prototype);
    Object.defineProperty(o, S.owner, { value: el });
    return new Proxy(o, datasetHandler);
});

/* ---------------- CSSStyleDeclaration ---------------- */

let knownProps = null;
const whiteSpaceNames = { 'collapse wrap': 'normal', 'collapse nowrap': 'nowrap', 'preserve nowrap': 'pre', 'preserve wrap': 'pre-wrap',
    'preserve-breaks wrap': 'pre-line', 'break-spaces wrap': 'break-spaces', collapse: 'normal', wrap: 'normal', nowrap: 'nowrap',
    preserve: 'pre-wrap', 'preserve-breaks': 'pre-line' };
const shorthands = ['margin', 'padding', 'border', 'border-top', 'border-right', 'border-bottom', 'border-left',
    'border-width', 'border-style', 'border-color', 'border-radius', 'background', 'font', 'list-style', 'outline',
    'flex', 'flex-flow', 'grid', 'grid-area', 'grid-row', 'grid-column', 'grid-template', 'gap', 'overflow',
    'text-decoration', 'transition', 'animation', 'columns', 'inset', 'place-items', 'place-content', 'place-self',
    'border-block', 'border-inline', 'margin-block', 'margin-inline', 'padding-block', 'padding-inline'];
function cssKnown(name) {
    if (!knownProps) knownProps = new Set(N.cssPropertyNames().concat(N.cssPropertyNames(true), shorthands, ['float']));
    return knownProps.has(name) || name.startsWith('--') || name.startsWith('-webkit-');
}
const dashed = (p) => {
    if (p === 'cssFloat') return 'float';
    if (p.startsWith('--')) return p;
    let d = p.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
    if (/^(webkit|moz|ms|o)-/.test(d)) d = '-' + d;
    return d;
};
class CSSStyleDeclaration {
    constructor() { throw new TypeError('Illegal constructor'); }
    get cssText() { return this._el ? N.styleText(this._el) : this._computedText(); }
    set cssText(v) { if (this._ro) throw domErr('Read only', 'NoModificationAllowedError'); this._el.setAttribute('style', str(v)); }
    get length() { return this._names().length; }
    item(i) { return this._names()[i >>> 0] || ''; }
    _names() { return this._el ? N.styleNames(this._el) : N.cssPropertyNames(); }
    _computedText() { return this._names().map((n) => n + ': ' + this.getPropertyValue(n) + ';').join(' '); }
    getPropertyValue(p) {
        p = str(p);
        if (!p.startsWith('--')) p = p.toLowerCase();
        const v = this._comp ? computedValue(this._comp, p, this._pseudo) : N.styleGet(this._el, p, 0);
        /* white-space reads as its one-keyword form where there is one */
        return p === 'white-space' && whiteSpaceNames[v] ? whiteSpaceNames[v] : v;
    }
    getPropertyPriority(p) { return this._el ? N.styleGet(this._el, str(p), 1) : ''; }
    setProperty(p, v, prio) {
        if (this._ro) throw domErr('Read only', 'NoModificationAllowedError');
        if (prio && str(prio).toLowerCase() !== 'important') return;
        N.styleSet(this._el, str(p), v === null || v === undefined ? '' : str(v), prio ? str(prio) : undefined);
    }
    removeProperty(p) {
        if (this._ro) throw domErr('Read only', 'NoModificationAllowedError');
        const old = this.getPropertyValue(p);
        N.styleSet(this._el, str(p), '', undefined);
        return old;
    }
    get parentRule() { return null; }
    get cssFloat() { return this.getPropertyValue('float'); }
    set cssFloat(v) { this.setProperty('float', v); }
    [Symbol.iterator]() { return this._names()[Symbol.iterator](); }
}
tag(CSSStyleDeclaration);
defGlobal('CSSStyleDeclaration', CSSStyleDeclaration);
const styleHandler = {
    get(t, p) {
        if (typeof p !== 'string' || p in t) return Reflect.get(t, p, t);
        if (isIndex(p)) return t.item(+p);
        const d = dashed(p);
        if (!cssKnown(d)) return undefined;
        return t.getPropertyValue(d);
    },
    set(t, p, v) {
        if (typeof p !== 'string' || p in t) return Reflect.set(t, p, v, t);
        const d = dashed(p);
        if (!cssKnown(d)) return Reflect.set(t, p, v, t);
        if (t._ro) return true;
        t.setProperty(d, v === null ? '' : v);
        return true;
    },
    has(t, p) { return typeof p === 'string' && !(p in t) ? cssKnown(dashed(p)) : p in t; },
};
function styleDecl(el, comp, pseudo) {
    const o = Object.create(CSSStyleDeclaration.prototype);
    Object.defineProperties(o, { _el: { value: comp ? null : el }, _comp: { value: comp ? el : null }, _ro: { value: !!comp },
        _pseudo: { value: pseudo | 0 } });
    return new Proxy(o, styleHandler);
}
/* A getComputedStyle value. width and height are the used size of the
 * element's box in pixels, as the standard's resolved values are (not
 * for inline boxes, display: none or a pseudo-element, which give the
 * computed value). An element without the pseudo-element asked for has
 * content: none. Margins, insets and percentages elsewhere stay computed. */
const replacedTags = /^(img|input|textarea|select|button|canvas|video|iframe|object|embed|svg|meter|progress)$/;
function computedValue(el, p, pseudo) {
    if (pseudo) {
        const v = N.computed(el, p, pseudo);
        return v === '' && p === 'content' && N.computed(el, 'content', pseudo) === '' ? 'none' : v;
    }
    const v = N.computed(el, p);
    if ((p === 'width' || p === 'height') && el.isConnected) {
        const display = N.computed(el, 'display');
        if (display === 'none' || display === 'contents' || (display === 'inline' && !replacedTags.test(el.localName))) return v;
        const r = N.rect(el);
        if (!r) return v;
        let size = p === 'width' ? r[2] : r[3];
        if (N.computed(el, 'box-sizing') !== 'border-box') {
            const sides = p === 'width' ? ['left', 'right'] : ['top', 'bottom'];
            for (const s of sides) {
                const pad = N.computed(el, 'padding-' + s), bw = N.computed(el, 'border-' + s + '-width');
                if (!/px$/.test(pad) || !/px$/.test(bw)) return v;
                size -= parseFloat(pad) + parseFloat(bw);
            }
        }
        return Math.max(0, Math.round(size * 1000) / 1000) + 'px';
    }
    return v;
}
const styleOf = (el) => cachedList(el, 'style', () => styleDecl(el, false));
defGlobal('getComputedStyle', function getComputedStyle(el, pseudo) {
    if (!isElement(el)) throw new TypeError("getComputedStyle: argument is not an Element");
    let which = 0;
    if (pseudo !== undefined && pseudo !== null && pseudo !== '') {
        const m = /^::?(before|after|marker)$/i.exec(str(pseudo).trim());
        if (m) which = { before: 1, after: 2, marker: 3 }[m[1].toLowerCase()];
        else if (/^::/.test(str(pseudo).trim())) which = 4; /* a pseudo-element Aura does not style: every value is "" */
    }
    return styleDecl(el, true, which);
});
defGlobal('CSS', {
    supports: (a, b) => (b === undefined ? N.cssSupports(str(a)) : N.cssSupports(str(a), str(b))),
    /* cssom.js keeps the registrations (an @property sheet of its own) */
    registerProperty(def) { cssom().registerProperty(def); },
    escape: (s) => str(s).replace(/[\0-\x1f\x7f]|^-?\d|^-$|[^\w\u0080-￿-]/g, (c) =>
        c === '\0' ? '�' : /[\0-\x1f\x7f]|\d/.test(c) ? '\\' + c.charCodeAt(0).toString(16) + ' ' : '\\' + c),
});

/* ---------------- geometry ---------------- */

class DOMRectReadOnly {
    constructor(x = 0, y = 0, w = 0, h = 0) { this._x = +x; this._y = +y; this._w = +w; this._h = +h; }
    get x() { return this._x; }
    get y() { return this._y; }
    get width() { return this._w; }
    get height() { return this._h; }
    get left() { return Math.min(this._x, this._x + this._w); }
    get top() { return Math.min(this._y, this._y + this._h); }
    get right() { return Math.max(this._x, this._x + this._w); }
    get bottom() { return Math.max(this._y, this._y + this._h); }
    toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, right: this.right, bottom: this.bottom, left: this.left }; }
    static fromRect(r) { r = r || {}; return new this(r.x, r.y, r.width, r.height); }
}
class DOMRect extends DOMRectReadOnly {
    set x(v) { this._x = +v; } get x() { return this._x; }
    set y(v) { this._y = +v; } get y() { return this._y; }
    set width(v) { this._w = +v; } get width() { return this._w; }
    set height(v) { this._h = +v; } get height() { return this._h; }
}
tag(DOMRectReadOnly); tag(DOMRect);
defGlobal('DOMRectReadOnly', DOMRectReadOnly);
defGlobal('DOMRect', DOMRect);

/* ---------------- the Console's Trace Scrolls ---------------- */

/* With Trace Scrolls on (Console window, au_page_set_trace), every
 * scroll script makes, every scroll event and every ResizeObserver
 * delivery is printed, with the page's code that made it: Discord's
 * message list keeps its own copy of the scroller's size and only
 * re-reads it when told, so its scroll jumps come from those three. */
let traceScroll = false;
P.setTrace = function (flags) { traceScroll = !!(flags & 1); };
function traceName(el) {
    if (!el || el.nodeType !== 1) return String(el);
    let s = el.localName;
    const id = el.getAttribute('id'), cls = el.getAttribute('class');
    if (id) s += '#' + id;
    if (cls) s += '.' + cls.trim().split(/\s+/)[0];
    return s;
}
/* The innermost frame of the page's own code, "host/path.js:line:column",
 * or "(Aura)" for the console's input line and Aura's own calls. */
function traceCaller() {
    const lines = String(new Error().stack || '').split('\n');
    for (const l of lines) {
        const m = /https?:\/\/([^\s)]+)/.exec(l);
        if (m) return m[1].replace(/\?[^:]*/, '');
    }
    return '(Aura)';
}
function trace(text) { N.print(0, '[trace] ' + text + ' <- ' + traceCaller()); }
const traceInt = (v) => Math.round(v * 10) / 10;
{
    const rawElScrollTo = N.elScrollTo, rawScrollTo = N.scrollTo;
    N.elScrollTo = function (el, x, y, by) {
        if (!traceScroll) return rawElScrollTo(el, x, y, by);
        const a = N.elScroll(el);
        const r = rawElScrollTo(el, x, y, by);
        const b = N.elScroll(el);
        if (a && b)
            trace('scroll ' + traceName(el) + ' to ' + traceInt(x) + ',' + traceInt(y) + (by ? ' (by)' : '') +
                  ': was ' + traceInt(a[0]) + ',' + traceInt(a[1]) + ' now ' + traceInt(b[0]) + ',' + traceInt(b[1]) +
                  ' of ' + traceInt(b[2]) + 'x' + traceInt(b[3]) + ' in ' + traceInt(b[4]) + 'x' + traceInt(b[5]));
        return r;
    };
    N.scrollTo = function (x, y) {
        if (!traceScroll) return rawScrollTo(x, y);
        const a = N.viewport(true);
        const r = rawScrollTo(x, y);
        const b = N.viewport(true);
        trace('scroll the page to ' + traceInt(x) + ',' + traceInt(y) + ': was ' + a[2] + ',' + a[3] + ' now ' +
              b[2] + ',' + b[3] + ' of ' + b[4] + 'x' + b[5]);
        return r;
    };
}

function docRect(el) { return N.rect(el); }
function viewportRect(el) {
    const r = N.rect(el, true); /* where it is drawn: scrolled, transformed */
    if (!r) return new DOMRect(0, 0, 0, 0);
    const v = N.viewport();
    return new DOMRect(r[0] - v[2], r[1] - v[3], r[2], r[3]);
}
const px = (el, prop) => parseFloat(N.computed(el, prop)) || 0;
const isRootish = (el) => el === doc.documentElement || (el === doc.body && doc.compatMode === 'BackCompat');

function offsetParentOf(el) {
    if (!el.isConnected || el === doc.body || el === doc.documentElement) return null;
    if (N.computed(el, 'display') === 'none') return null;
    for (let p = el.parentElement; p; p = p.parentElement) {
        if (p === doc.body) return p;
        const pos = N.computed(p, 'position');
        if (pos !== 'static' && pos !== '') return p;
        if (/^(td|th|table)$/.test(p.localName) && N.computed(el, 'position') === 'static') return p;
    }
    return doc.body;
}

defProps(EP, {
    getBoundingClientRect() { return viewportRect(this); },
    getClientRects() { const r = docRect(this); return r ? [viewportRect(this)] : []; },
    get clientWidth() {
        if (isRootish(this)) return N.viewport()[0];
        const r = docRect(this);
        if (!r || N.computed(this, 'display') === 'inline') return 0;
        return Math.max(0, Math.round(r[2] - px(this, 'border-left-width') - px(this, 'border-right-width')));
    },
    get clientHeight() {
        if (isRootish(this)) return N.viewport()[1];
        const r = docRect(this);
        if (!r || N.computed(this, 'display') === 'inline') return 0;
        return Math.max(0, Math.round(r[3] - px(this, 'border-top-width') - px(this, 'border-bottom-width')));
    },
    get clientTop() { return Math.round(px(this, 'border-top-width')); },
    get clientLeft() { return Math.round(px(this, 'border-left-width')); },
    /* An element whose box clips (overflow auto, scroll or hidden) has its
     * own scroll offset (N.elScroll); the root and body are the page's. */
    get scrollWidth() {
        if (isRootish(this) || this === doc.body) return N.viewport(true)[4];
        const s = N.elScroll(this);
        return s ? Math.round(s[2]) : this.clientWidth;
    },
    get scrollHeight() {
        if (isRootish(this) || this === doc.body) return N.viewport(true)[5];
        const s = N.elScroll(this);
        return s ? Math.round(s[3]) : this.clientHeight;
    },
    get scrollTop() {
        if (isRootish(this) || this === doc.body) return N.viewport()[3];
        const s = N.elScroll(this);
        return s ? s[1] : 0;
    },
    set scrollTop(v) {
        if (isRootish(this) || this === doc.body) { N.scrollTo(N.viewport()[2], +v || 0); return; }
        const s = N.elScroll(this);
        if (s) N.elScrollTo(this, s[0], +v || 0);
    },
    get scrollLeft() {
        if (isRootish(this) || this === doc.body) return N.viewport()[2];
        const s = N.elScroll(this);
        return s ? s[0] : 0;
    },
    set scrollLeft(v) {
        if (isRootish(this) || this === doc.body) { N.scrollTo(+v || 0, N.viewport()[3]); return; }
        const s = N.elScroll(this);
        if (s) N.elScrollTo(this, +v || 0, s[1]);
    },
    /* Scrolls each scroll container the element is in, innermost first,
     * then the page (CSSOM View, "scroll an element into view"). MDN
     * scrolls its sidebar to the current page's entry this way. */
    scrollIntoView(arg) {
        if (!N.rect(this)) return;
        const o = arg && typeof arg === 'object' ? arg : {};
        const block = o.block || (arg === false ? 'end' : 'start');
        const inline = o.inline || 'nearest';
        const delta = (lo, size, vlo, vsize, how) => {
            if (how === 'end') return lo + size - (vlo + vsize);
            if (how === 'center') return lo + size / 2 - (vlo + vsize / 2);
            if (how === 'nearest') return lo < vlo ? lo - vlo : lo + size > vlo + vsize ? lo + size - (vlo + vsize) : 0;
            return lo - vlo;
        };
        for (let p = this.parentNode, n = 0; p && n < 1000; n++) {
            if (p.nodeType === 11) { p = p.host || null; continue; } /* out of a shadow tree */
            if (p.nodeType !== 1 || isRootish(p) || p === doc.body) { p = p.parentNode; continue; }
            const s = N.elScroll(p);
            if (s && (s[2] > s[4] || s[3] > s[5])) {
                const r = N.rect(this, true), pr = N.rect(p, true);
                if (r && pr) {
                    const vx = pr[0] + px(p, 'border-left-width'), vy = pr[1] + px(p, 'border-top-width');
                    N.elScrollTo(p, s[0] + delta(r[0], r[2], vx, s[4], inline), s[1] + delta(r[1], r[3], vy, s[5], block));
                }
            }
            p = p.parentNode;
        }
        const r = N.rect(this, true);
        if (!r) return;
        const v = N.viewport();
        N.scrollTo(Math.max(0, v[2] + delta(r[0], r[2], v[2], v[0], inline)),
                   Math.max(0, v[3] + delta(r[1], r[3], v[3], v[1], block)));
    },
    scrollTo(x, y) {
        if (isRootish(this) || this === doc.body) { G.scrollTo(x, y); return; }
        const s = N.elScroll(this);
        if (!s) return;
        if (x && typeof x === 'object') {
            N.elScrollTo(this, x.left !== undefined ? +x.left || 0 : s[0], x.top !== undefined ? +x.top || 0 : s[1]);
        } else if (x !== undefined) {
            N.elScrollTo(this, +x || 0, +y || 0);
        }
    },
    scroll(x, y) { this.scrollTo(x, y); },
    scrollBy(x, y) {
        if (isRootish(this) || this === doc.body) { G.scrollBy(x, y); return; }
        const s = N.elScroll(this);
        if (!s) return;
        if (x && typeof x === 'object') N.elScrollTo(this, s[0] + (+x.left || 0), s[1] + (+x.top || 0), true);
        else N.elScrollTo(this, s[0] + (+x || 0), s[1] + (+y || 0), true);
    },
});
defProps(HTMLElement.prototype, {
    get offsetParent() { return offsetParentOf(this); },
    get offsetTop() {
        const r = docRect(this);
        if (!r) return 0;
        const p = offsetParentOf(this);
        const pr = p && p !== doc.body ? docRect(p) : null;
        return Math.round(r[1] - (pr ? pr[1] + px(p, 'border-top-width') : 0));
    },
    get offsetLeft() {
        const r = docRect(this);
        if (!r) return 0;
        const p = offsetParentOf(this);
        const pr = p && p !== doc.body ? docRect(p) : null;
        return Math.round(r[0] - (pr ? pr[0] + px(p, 'border-left-width') : 0));
    },
    get offsetWidth() { const r = docRect(this); return r ? Math.round(r[2]) : 0; },
    get offsetHeight() { const r = docRect(this); return r ? Math.round(r[3]) : 0; },
});

/* ---------------- focus ---------------- */

let focused = null;
const focusableTags = /^(a|area|button|input|select|textarea|iframe|summary)$/;
function isFocusable(el) {
    if (!isElement(el) || !el.isConnected || N.inert(el)) return false;
    if (el.hasAttribute('tabindex')) return true;
    if (el.hasAttribute('contenteditable')) return true;
    if (!focusableTags.test(el.localName) || el.namespaceURI !== HTML_NS) return false;
    if ((el.localName === 'a' || el.localName === 'area') && !el.hasAttribute('href')) return false;
    if (el.hasAttribute('disabled')) return false;
    if (el.localName === 'input' && (el.getAttribute('type') || '').toLowerCase() === 'hidden') return false;
    return true;
}
function setFocus(el) {
    const old = focused;
    if (old === el) return;
    if (old) {
        focused = null;
        fire(old, new FocusEvent('blur', { relatedTarget: el, composed: true }));
        fire(old, new FocusEvent('focusout', { bubbles: true, relatedTarget: el, composed: true }));
    }
    focused = el;
    N.setFocus(el, old);
    if (el && focused === el) {
        fire(el, new FocusEvent('focus', { relatedTarget: old, composed: true }));
        fire(el, new FocusEvent('focusin', { bubbles: true, relatedTarget: old, composed: true }));
    }
    editFocusMoved(); /* prelude_obs.js: an editing host's caret */
}
/* A host whose shadow root delegates focus hands it to the first
 * focusable element of its shadow tree (shadow-including order), unless
 * the focus is already in there. */
function delegateTarget(host) {
    const root = N.shadowRootOf(host);
    if (!root || !root.delegatesFocus) return null;
    if (focused && focused.isConnected && shadowIncludingAncestor(root, focused)) return focused;
    const stack = [root];
    while (stack.length) {
        const n = stack.pop();
        if (n !== root && isFocusable(n)) return n;
        const kids = [];
        if (n !== root && isElement(n)) { const r = N.shadowRootOf(n); if (r) kids.push(r); }
        for (let c = n.firstChild; c; c = c.nextSibling) if (isElement(c)) kids.push(c);
        for (let i = kids.length - 1; i >= 0; i--) stack.push(kids[i]);
    }
    return null;
}
function focusElement(el) {
    if (isElement(el) && !el.hasAttribute('tabindex')) {
        const d = delegateTarget(el);
        if (d) { if (d !== focused) setFocus(d); return; }
    }
    if (isFocusable(el)) setFocus(el);
}
defProps(HTMLElement.prototype, {
    focus() { focusElement(this); },
    blur() { if (focused === this) setFocus(null); },
});
defProps(SVGElement.prototype, {
    focus() { focusElement(this); },
    blur() { if (focused === this) setFocus(null); },
});
defProps(DP, {
    get activeElement() {
        if (focused && !focused.isConnected) focused = null;
        /* the focused element, or its host in this document's tree */
        return (focused && retarget(focused, this)) || doc.body || doc.documentElement;
    },
    hasFocus() { return true; },
});
if (typeof ShadowRoot === 'function') {
    defProps(ShadowRoot.prototype, {
        get activeElement() {
            if (!focused || !focused.isConnected || !shadowIncludingAncestor(this, focused)) return null;
            const t = retarget(focused, this);
            return t && t.getRootNode() === this ? t : null;
        },
        elementFromPoint(x, y) {
            const e = N.hitTest(+x, +y);
            if (!e || !shadowIncludingAncestor(this, e)) return null;
            const t = retarget(e, this);
            return t && t.getRootNode() === this ? t : null;
        },
        elementsFromPoint(x, y) {
            const e = this.elementFromPoint(x, y);
            const out = [];
            for (let n = e; n && n !== this; n = n.parentNode) if (isElement(n)) out.push(n);
            return out;
        },
    });
}
