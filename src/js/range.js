
/* Range and Selection. The build compiles this file to its own bytecode
 * (range.bc); the prelude reads it with N.lazy(0) the first time script
 * touches Range, AbstractRange, StaticRange, Selection, getSelection or
 * document.createRange, so a page that never does pays only for the
 * getters. It is a function of G, the global object, and H, the prelude's
 * helpers, and returns the classes and getSelection.
 *
 * Range follows the DOM standard. Its boundary points live in C
 * (ranges.c), where the mutation hooks move them when nodes are inserted
 * or removed; character data edits report through replaceData and
 * splitText in prelude_dom.js. Offsets are UTF-16 code units.
 *
 * getClientRects and getBoundingClientRect ask layout for the rectangles
 * of the selected part of each text node (N.textRects).
 * Not exact: normalize() moves boundary points in merged text nodes to
 * their parent. The Selection object is script state: it is not the
 * selection the user makes with the mouse, and it is not painted. */
(function (G, H) {
'use strict';

const { N, doc, DOMRect, Event, S, domErr, fire, isNode, rootOf, str, tag } = H;

class AbstractRange {
    constructor() { if (new.target === AbstractRange) throw new TypeError('Illegal constructor'); }
}
const isCharData = (n) => { const t = n.nodeType; return t === 3 || t === 4 || t === 7 || t === 8; };
const nodeLength = (n) => (n.nodeType === 10 ? 0 : isCharData(n) ? n.data.length : n.childNodes.length);
const nodeIndex = (n) => { let i = 0; for (let c = n.previousSibling; c; c = c.previousSibling) i++; return i; };
const isAncestorOrSelf = (a, n) => a === n || a.contains(n);
/* The position of boundary point (nA, oA) relative to (nB, oB): -1
 * before, 0 equal, 1 after. Both nodes have the same root. */
function comparePoints(nA, oA, nB, oB) {
    if (nA === nB) return oA === oB ? 0 : oA < oB ? -1 : 1;
    const pos = nA.compareDocumentPosition(nB);
    if (pos & 2) return -comparePoints(nB, oB, nA, oA); /* nA follows nB */
    if (pos & 16) { /* nA is an ancestor of nB */
        let child = nB;
        while (child.parentNode !== nA) child = child.parentNode;
        if (nodeIndex(child) < oA) return 1;
    }
    return -1;
}
const rangeFinalizer = typeof FinalizationRegistry === 'function' ? new FinalizationRegistry((id) => N.rangeFree(id)) : null;
const rangeNode = (n, what) => { if (!isNode(n)) throw new TypeError(what + ': argument is not a Node'); return n; };

class Range extends AbstractRange {
    constructor() {
        super();
        const id = N.rangeNew();
        Object.defineProperty(this, S.range, { value: id });
        if (rangeFinalizer) rangeFinalizer.register(this, id);
    }
    get startContainer() { return N.rangeGet(this[S.range])[0]; }
    get startOffset() { return N.rangeGet(this[S.range])[1]; }
    get endContainer() { return N.rangeGet(this[S.range])[2]; }
    get endOffset() { return N.rangeGet(this[S.range])[3]; }
    get collapsed() { const p = N.rangeGet(this[S.range]); return p[0] === p[2] && p[1] === p[3]; }
    get commonAncestorContainer() {
        const p = N.rangeGet(this[S.range]);
        let c = p[0];
        while (!isAncestorOrSelf(c, p[2])) c = c.parentNode;
        return c;
    }
    setStart(node, offset) { setBoundary(this, rangeNode(node, 'setStart'), offset >>> 0, true); }
    setEnd(node, offset) { setBoundary(this, rangeNode(node, 'setEnd'), offset >>> 0, false); }
    setStartBefore(node) { const p = parentFor(node, 'setStartBefore'); setBoundary(this, p, nodeIndex(node), true); }
    setStartAfter(node) { const p = parentFor(node, 'setStartAfter'); setBoundary(this, p, nodeIndex(node) + 1, true); }
    setEndBefore(node) { const p = parentFor(node, 'setEndBefore'); setBoundary(this, p, nodeIndex(node), false); }
    setEndAfter(node) { const p = parentFor(node, 'setEndAfter'); setBoundary(this, p, nodeIndex(node) + 1, false); }
    collapse(toStart) {
        const p = N.rangeGet(this[S.range]);
        if (toStart) N.rangeSet(this[S.range], p[0], p[1], p[0], p[1]);
        else N.rangeSet(this[S.range], p[2], p[3], p[2], p[3]);
    }
    selectNode(node) {
        const p = parentFor(node, 'selectNode');
        const i = nodeIndex(node);
        N.rangeSet(this[S.range], p, i, p, i + 1);
    }
    selectNodeContents(node) {
        rangeNode(node, 'selectNodeContents');
        if (node.nodeType === 10) throw domErr('The node is a doctype', 'InvalidNodeTypeError');
        N.rangeSet(this[S.range], node, 0, node, nodeLength(node));
    }
    compareBoundaryPoints(how, source) {
        how = Number(how) & 0xffff;
        if (!(source instanceof Range)) throw new TypeError('compareBoundaryPoints: argument 2 is not a Range');
        if (how > 3) throw domErr('The comparison method is not supported', 'NotSupportedError');
        const a = N.rangeGet(this[S.range]), b = N.rangeGet(source[S.range]);
        if (rootOf(a[0]) !== rootOf(b[0])) throw domErr('The ranges are in different trees', 'WrongDocumentError');
        switch (how) {
        case 0: return comparePoints(a[0], a[1], b[0], b[1]);
        case 1: return comparePoints(a[2], a[3], b[0], b[1]);
        case 2: return comparePoints(a[2], a[3], b[2], b[3]);
        default: return comparePoints(a[0], a[1], b[2], b[3]);
        }
    }
    comparePoint(node, offset) {
        rangeNode(node, 'comparePoint');
        offset >>>= 0;
        const p = N.rangeGet(this[S.range]);
        if (rootOf(node) !== rootOf(p[0])) throw domErr('The node is in a different tree', 'WrongDocumentError');
        if (node.nodeType === 10) throw domErr('The node is a doctype', 'InvalidNodeTypeError');
        if (offset > nodeLength(node)) throw domErr('The offset is out of range', 'IndexSizeError');
        if (comparePoints(node, offset, p[0], p[1]) < 0) return -1;
        if (comparePoints(node, offset, p[2], p[3]) > 0) return 1;
        return 0;
    }
    isPointInRange(node, offset) {
        rangeNode(node, 'isPointInRange');
        offset >>>= 0;
        const p = N.rangeGet(this[S.range]);
        if (rootOf(node) !== rootOf(p[0])) return false;
        if (node.nodeType === 10) throw domErr('The node is a doctype', 'InvalidNodeTypeError');
        if (offset > nodeLength(node)) throw domErr('The offset is out of range', 'IndexSizeError');
        return comparePoints(node, offset, p[0], p[1]) >= 0 && comparePoints(node, offset, p[2], p[3]) <= 0;
    }
    intersectsNode(node) {
        rangeNode(node, 'intersectsNode');
        const p = N.rangeGet(this[S.range]);
        if (rootOf(node) !== rootOf(p[0])) return false;
        const parent = node.parentNode;
        if (!parent) return true;
        const i = nodeIndex(node);
        return comparePoints(parent, i, p[2], p[3]) < 0 && comparePoints(parent, i + 1, p[0], p[1]) > 0;
    }
    cloneRange() {
        const r = new Range(), p = N.rangeGet(this[S.range]);
        N.rangeSet(r[S.range], p[0], p[1], p[2], p[3]);
        return r;
    }
    detach() {}
    toString() {
        const p = N.rangeGet(this[S.range]);
        const [sn, so, en, eo] = p;
        if (sn === en && sn.nodeType === 3) return sn.data.slice(so, eo);
        let s = '';
        if (sn.nodeType === 3) s += sn.data.slice(so);
        for (const n of containedNodes(p, true)) if (n.nodeType === 3) s += n.data;
        if (en.nodeType === 3) s += en.data.slice(0, eo);
        return s;
    }
    deleteContents() {
        const p = N.rangeGet(this[S.range]);
        const [sn, so, en, eo] = p;
        if (sn === en && so === eo) return;
        if (sn === en && isCharData(sn)) { sn.replaceData(so, eo - so, ''); return; }
        const remove = containedNodes(p, false);
        const [nn, no] = pointAfterExtract(sn, so, en);
        if (isCharData(sn)) sn.replaceData(so, nodeLength(sn) - so, '');
        for (const n of remove) if (n.parentNode) n.parentNode.removeChild(n);
        if (isCharData(en)) en.replaceData(0, eo, '');
        N.rangeSet(this[S.range], nn, no, nn, no);
    }
    extractContents() { return copyContents(this, true); }
    cloneContents() { return copyContents(this, false); }
    insertNode(node) {
        rangeNode(node, 'insertNode');
        const [sn, so] = N.rangeGet(this[S.range]);
        if (sn.nodeType === 7 || sn.nodeType === 8 || (sn.nodeType === 3 && !sn.parentNode) || sn === node)
            throw domErr('The node cannot be inserted at the start of the range', 'HierarchyRequestError');
        let ref = sn.nodeType === 3 ? sn : sn.childNodes[so] || null;
        const parent = ref ? ref.parentNode : sn;
        /* insertBefore validates the hierarchy and throws as the standard asks */
        if (sn.nodeType === 3) ref = sn.splitText(so);
        if (node === ref) ref = ref.nextSibling;
        if (node.parentNode) node.parentNode.removeChild(node);
        let newOffset = ref ? nodeIndex(ref) : nodeLength(parent);
        newOffset += node.nodeType === 11 ? nodeLength(node) : 1;
        parent.insertBefore(node, ref);
        const p = N.rangeGet(this[S.range]);
        if (p[0] === p[2] && p[1] === p[3]) N.rangeSet(this[S.range], p[0], p[1], parent, newOffset);
    }
    surroundContents(newParent) {
        rangeNode(newParent, 'surroundContents');
        const p = N.rangeGet(this[S.range]);
        /* a non-Text node partially contained */
        for (const [a, b] of [[p[0], p[2]], [p[2], p[0]]]) {
            for (let n = a; n && !isAncestorOrSelf(n, b); n = n.parentNode)
                if (n.nodeType !== 3) throw domErr('The range partially contains a node that is not text', 'InvalidStateError');
        }
        const t = newParent.nodeType;
        if (t === 9 || t === 10 || t === 11) throw domErr('The new parent cannot be a document, doctype or fragment', 'InvalidNodeTypeError');
        const f = this.extractContents();
        while (newParent.firstChild) newParent.removeChild(newParent.firstChild);
        this.insertNode(newParent);
        newParent.appendChild(f);
        this.selectNode(newParent);
    }
    createContextualFragment(html) {
        const sn = N.rangeGet(this[S.range])[0];
        const ctx = sn.nodeType === 1 ? sn : sn.parentElement || doc.body;
        const t = ctx && ctx.localName !== 'html' ? ctx.cloneNode(false) : doc.createElement('body');
        t.innerHTML = str(html);
        const f = doc.createDocumentFragment();
        while (t.firstChild) f.appendChild(t.firstChild);
        return f;
    }
    getClientRects() {
        /* CSSOM View: the border boxes of each element the range selects
         * whose parent it does not select, and for each text node it
         * selects or partly selects the rectangles of the selected part
         * (layout answers per line, N.textRects; for a collapsed range in
         * text, one rectangle of no width). Where layout cannot answer for
         * a text node (no hook), its parent element's boxes stand in. */
        const p = N.rangeGet(this[S.range]);
        const out = [];
        const v = N.viewport();
        const stood = new Set();
        const text = (n, a, b) => {
            const r = N.textRects(n, a, b);
            if (r) { for (let i = 0; i < r.length; i += 4) out.push(new DOMRect(r[i] - v[2], r[i + 1] - v[3], r[i + 2], r[i + 3])); return; }
            const el = n.parentElement;
            if (r === undefined && el && a !== b && !stood.has(el)) {
                stood.add(el);
                for (const q of el.getClientRects()) out.push(q);
            }
        };
        if (p[0] === p[2]) {
            if (p[0].nodeType === 3) text(p[0], p[1], p[3]);
            if (p[0].nodeType === 3 || p[1] === p[3]) return out;
        }
        if (p[0].nodeType === 3) text(p[0], p[1], p[0].data.length);
        const all = containedNodes(p, true), set = new Set(all);
        for (const n of all) {
            if (n.nodeType === 1 && !set.has(n.parentNode)) for (const q of n.getClientRects()) out.push(q);
            else if (n.nodeType === 3) text(n, 0, n.data.length);
        }
        if (p[2].nodeType === 3 && p[2] !== p[0]) text(p[2], 0, p[3]);
        return out;
    }
    getBoundingClientRect() {
        const rs = this.getClientRects();
        if (!rs.length) {
            /* a collapsed range: the box of the element at its start, without area */
            const sn = N.rangeGet(this[S.range])[0];
            const el = sn.nodeType === 1 ? sn : sn.parentElement;
            if (!el || !sn.isConnected) return new DOMRect();
            const r = el.getBoundingClientRect();
            return new DOMRect(r.left, r.top, 0, r.height);
        }
        let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity;
        for (const q of rs) { l = Math.min(l, q.left); t = Math.min(t, q.top); r = Math.max(r, q.right); b = Math.max(b, q.bottom); }
        return new DOMRect(l, t, r - l, b - t);
    }
}
for (const [k, v] of [['START_TO_START', 0], ['START_TO_END', 1], ['END_TO_END', 2], ['END_TO_START', 3]]) {
    Object.defineProperty(Range, k, { value: v, enumerable: true });
    Object.defineProperty(Range.prototype, k, { value: v, enumerable: true });
}
function parentFor(node, what) {
    rangeNode(node, what);
    const p = node.parentNode;
    if (!p) throw domErr('The node has no parent', 'InvalidNodeTypeError');
    return p;
}
function setBoundary(range, node, offset, start) {
    if (node.nodeType === 10) throw domErr('The node is a doctype', 'InvalidNodeTypeError');
    if (offset > nodeLength(node)) throw domErr('The offset is out of range', 'IndexSizeError');
    const id = range[S.range], p = N.rangeGet(id);
    const sameRoot = rootOf(p[0]) === rootOf(node);
    if (start) {
        if (!sameRoot || comparePoints(node, offset, p[2], p[3]) > 0) N.rangeSet(id, node, offset, node, offset);
        else N.rangeSet(id, node, offset, p[2], p[3]);
    } else if (!sameRoot || comparePoints(node, offset, p[0], p[1]) < 0) {
        N.rangeSet(id, node, offset, node, offset);
    } else {
        N.rangeSet(id, p[0], p[1], node, offset);
    }
}
/* The nodes contained in the range p (rangeGet's array), in tree order:
 * all of them (all true), or only those whose parent is not contained. A
 * node is contained when its start is after the range's start and its end
 * before the range's end. */
function containedNodes(p, all) {
    const [sn, so, en, eo] = p;
    let common = sn;
    while (!isAncestorOrSelf(common, en)) common = common.parentNode;
    const out = [];
    const contained = (n) => comparePoints(n, 0, sn, so) > 0 && comparePoints(n, nodeLength(n), en, eo) < 0;
    let n = common.firstChild;
    while (n) {
        let down = true;
        if (contained(n)) { out.push(n); down = all; }
        /* a subtree wholly before the start or after the end holds nothing */
        else if (!isAncestorOrSelf(n, sn) && !isAncestorOrSelf(n, en)) down = false;
        if (down && n.firstChild) { n = n.firstChild; continue; }
        while (n && n !== common && !n.nextSibling) n = n.parentNode;
        if (!n || n === common) break;
        n = n.nextSibling;
    }
    return out;
}
/* Where the range collapses to after its contents are removed. */
function pointAfterExtract(sn, so, en) {
    if (isAncestorOrSelf(sn, en)) return [sn, so];
    let ref = sn;
    while (ref.parentNode && !isAncestorOrSelf(ref.parentNode, en)) ref = ref.parentNode;
    return [ref.parentNode, nodeIndex(ref) + 1];
}
/* cloneContents (extract false) and extractContents (extract true). */
function copyContents(range, extract) {
    const [sn, so, en, eo] = N.rangeGet(range[S.range]);
    const od = sn.nodeType === 9 ? sn : sn.ownerDocument || doc;
    const frag = od.createDocumentFragment();
    if (sn === en && so === eo) return frag;
    const piece = (node, from, count) => {
        const c = node.cloneNode(false);
        c.data = node.data.substr(from, count);
        if (extract) node.replaceData(from, count, '');
        return c;
    };
    if (sn === en && isCharData(sn)) { frag.appendChild(piece(sn, so, eo - so)); return frag; }
    let common = sn;
    while (!isAncestorOrSelf(common, en)) common = common.parentNode;
    const partial = (n) => isAncestorOrSelf(n, sn) !== isAncestorOrSelf(n, en);
    let first = null, last = null;
    if (!isAncestorOrSelf(sn, en)) for (let c = common.firstChild; c && !first; c = c.nextSibling) if (partial(c)) first = c;
    if (!isAncestorOrSelf(en, sn)) for (let c = common.lastChild; c && !last; c = c.previousSibling) if (partial(c)) last = c;
    const inside = [];
    for (let c = common.firstChild; c; c = c.nextSibling)
        if (comparePoints(c, 0, sn, so) > 0 && comparePoints(c, nodeLength(c), en, eo) < 0) inside.push(c);
    for (const c of inside) if (c.nodeType === 10) throw domErr('The range contains a doctype', 'HierarchyRequestError');
    const after = extract ? pointAfterExtract(sn, so, en) : null;
    const sub = (a, ao, b, bo) => { const r = new Range(); N.rangeSet(r[S.range], a, ao, b, bo); return copyContents(r, extract); };
    if (first && isCharData(first)) {
        frag.appendChild(piece(sn, so, nodeLength(sn) - so));
    } else if (first) {
        const c = first.cloneNode(false);
        frag.appendChild(c);
        c.appendChild(sub(sn, so, first, nodeLength(first)));
    }
    for (const c of inside) frag.appendChild(extract ? c : c.cloneNode(true));
    if (last && isCharData(last)) {
        frag.appendChild(piece(en, 0, eo));
    } else if (last) {
        const c = last.cloneNode(false);
        frag.appendChild(c);
        c.appendChild(sub(last, 0, en, eo));
    }
    if (extract) N.rangeSet(range[S.range], after[0], after[1], after[0], after[1]);
    return frag;
}
tag(AbstractRange); tag(Range);

class StaticRange extends AbstractRange {
    constructor(init) {
        super();
        if (!init || !isNode(init.startContainer) || !isNode(init.endContainer)) throw new TypeError('StaticRange: startContainer and endContainer are required');
        for (const n of [init.startContainer, init.endContainer])
            if (n.nodeType === 10 || n.nodeType === 2) throw domErr('A boundary cannot be a doctype or an attribute', 'InvalidNodeTypeError');
        for (const k of ['startContainer', 'endContainer']) Object.defineProperty(this, k, { value: init[k], enumerable: true });
        for (const k of ['startOffset', 'endOffset']) Object.defineProperty(this, k, { value: init[k] >>> 0, enumerable: true });
    }
    get collapsed() { return this.startContainer === this.endContainer && this.startOffset === this.endOffset; }
}
tag(StaticRange);

/* One range at most, with a direction: the anchor is the range's start
 * unless the selection was made backwards. selectionchange fires at the
 * document, as a task, after a change. */
let selRange = null, selBackwards = false, selChangeQueued = false;
function selChanged() {
    if (selChangeQueued) return;
    selChangeQueued = true;
    N.queueTask(() => { selChangeQueued = false; fire(doc, new Event('selectionchange')); });
}
function selSet(r, backwards) {
    selRange = r; selBackwards = !!backwards && !!r; selChanged();
    H.selectionMoved(); /* the editing caret follows (prelude_obs.js) */
}
function selPoints() {
    const p = N.rangeGet(selRange[S.range]);
    return selBackwards ? [p[2], p[3], p[0], p[1]] : p;
}
function selCheck(node, offset, what) {
    rangeNode(node, what);
    if (node.nodeType === 10) throw domErr('The node is a doctype', 'InvalidNodeTypeError');
    if (offset > nodeLength(node)) throw domErr('The offset is out of range', 'IndexSizeError');
    return rootOf(node) === doc;
}
class Selection {
    constructor() { throw new TypeError('Illegal constructor'); }
    get rangeCount() { return selRange ? 1 : 0; }
    get isCollapsed() { return !selRange || selRange.collapsed; }
    get anchorNode() { return selRange ? selPoints()[0] : null; }
    get anchorOffset() { return selRange ? selPoints()[1] : 0; }
    get focusNode() { return selRange ? selPoints()[2] : null; }
    get focusOffset() { return selRange ? selPoints()[3] : 0; }
    get type() { return !selRange ? 'None' : selRange.collapsed ? 'Caret' : 'Range'; }
    get direction() { return !selRange || selRange.collapsed ? 'none' : selBackwards ? 'backward' : 'forward'; }
    getRangeAt(i) { if (!selRange || (i >>> 0) !== 0) throw domErr('No range at that index', 'IndexSizeError'); return selRange; }
    addRange(r) {
        if (!(r instanceof Range)) throw new TypeError('addRange: argument is not a Range');
        if (selRange || rootOf(r.startContainer) !== doc) return;
        selSet(r, false);
    }
    removeRange(r) {
        if (r !== selRange) throw domErr('The range is not in the selection', 'NotFoundError');
        selSet(null);
    }
    removeAllRanges() { if (selRange) selSet(null); }
    empty() { this.removeAllRanges(); }
    collapse(node, offset = 0) {
        if (node === null || node === undefined) { this.removeAllRanges(); return; }
        offset >>>= 0;
        if (!selCheck(node, offset, 'collapse')) return;
        const r = new Range();
        N.rangeSet(r[S.range], node, offset, node, offset);
        selSet(r, false);
    }
    setPosition(node, offset = 0) { this.collapse(node, offset); }
    collapseToStart() {
        if (!selRange) throw domErr('The selection is empty', 'InvalidStateError');
        const p = N.rangeGet(selRange[S.range]);
        this.collapse(p[0], p[1]);
    }
    collapseToEnd() {
        if (!selRange) throw domErr('The selection is empty', 'InvalidStateError');
        const p = N.rangeGet(selRange[S.range]);
        this.collapse(p[2], p[3]);
    }
    extend(node, offset = 0) {
        offset >>>= 0;
        if (!selRange) throw domErr('The selection is empty', 'InvalidStateError');
        if (!selCheck(node, offset, 'extend')) return;
        const [an, ao] = selPoints();
        const r = new Range();
        if (rootOf(an) !== rootOf(node)) { N.rangeSet(r[S.range], node, offset, node, offset); selSet(r, false); return; }
        if (comparePoints(an, ao, node, offset) <= 0) { N.rangeSet(r[S.range], an, ao, node, offset); selSet(r, false); }
        else { N.rangeSet(r[S.range], node, offset, an, ao); selSet(r, true); }
    }
    setBaseAndExtent(an, ao, fn, fo) {
        ao >>>= 0; fo >>>= 0;
        const ok = selCheck(an, ao, 'setBaseAndExtent') & selCheck(fn, fo, 'setBaseAndExtent');
        if (!ok) return;
        const r = new Range();
        if (comparePoints(an, ao, fn, fo) <= 0) { N.rangeSet(r[S.range], an, ao, fn, fo); selSet(r, false); }
        else { N.rangeSet(r[S.range], fn, fo, an, ao); selSet(r, true); }
    }
    selectAllChildren(node) {
        rangeNode(node, 'selectAllChildren');
        if (node.nodeType === 10) throw domErr('The node is a doctype', 'InvalidNodeTypeError');
        if (rootOf(node) !== doc) return;
        const r = new Range();
        N.rangeSet(r[S.range], node, 0, node, node.childNodes.length);
        selSet(r, false);
    }
    deleteFromDocument() { if (selRange) selRange.deleteContents(); }
    containsNode(node, partly = false) {
        rangeNode(node, 'containsNode');
        if (!selRange || rootOf(node) !== doc) return false;
        if (partly) return selRange.intersectsNode(node);
        /* the whole node: the points before and after it are in the range */
        const parent = node.parentNode;
        if (!parent) return false;
        const p = N.rangeGet(selRange[S.range]), i = nodeIndex(node);
        return comparePoints(parent, i, p[0], p[1]) >= 0 && comparePoints(parent, i + 1, p[2], p[3]) <= 0;
    }
    toString() { return selRange ? selRange.toString() : ''; }
}
tag(Selection);
const theSelection = Object.create(Selection.prototype);

return { AbstractRange, Range, StaticRange, Selection, getSelection: () => theSelection };
})
