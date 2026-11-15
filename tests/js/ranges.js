// Range (boundary points that follow DOM mutations, comparison, contents)
// and Selection, as the DOM and Selection API standards describe them.
'use strict';

function fixture(html) {
    const d = document.createElement('div');
    d.innerHTML = html;
    document.body.appendChild(d);
    return d;
}
const pts = (r) => [r.startContainer, r.startOffset, r.endContainer, r.endOffset];
function same(r, sc, so, ec, eo, msg) {
    assert.ok(r.startContainer === sc, (msg || '') + ' start container');
    assert.equal(r.startOffset, so, (msg || '') + ' start offset');
    assert.ok(r.endContainer === ec, (msg || '') + ' end container');
    assert.equal(r.endOffset, eo, (msg || '') + ' end offset');
}

test('a new range is collapsed at the document', () => {
    const r = new Range();
    same(r, document, 0, document, 0);
    assert.ok(r.collapsed);
    assert.ok(r instanceof AbstractRange);
    assert.equal(Range.END_TO_START, 3);
    assert.equal(r.START_TO_END, 1);
    assert.throws(() => new AbstractRange(), TypeError);
});

test('setStart and setEnd keep start before end', () => {
    const d = fixture('<p>one</p><p>two</p><p>three</p>');
    const [a, b, c] = d.children;
    const r = document.createRange();
    r.setStart(b.firstChild, 1);
    same(r, b.firstChild, 1, b.firstChild, 1, 'start past the end collapses');
    r.setEnd(c.firstChild, 2);
    same(r, b.firstChild, 1, c.firstChild, 2);
    r.setEnd(a.firstChild, 0);
    same(r, a.firstChild, 0, a.firstChild, 0, 'end before the start collapses');
    assert.throws(() => r.setStart(a.firstChild, 4), 'IndexSizeError');
    assert.throws(() => r.setStart(document.doctype || document.implementation.createHTMLDocument('x').doctype, 0), 'InvalidNodeTypeError');
    r.setStartBefore(b);
    r.setEndAfter(b);
    same(r, d, 1, d, 2);
    r.selectNode(c);
    same(r, d, 2, d, 3);
    r.selectNodeContents(c);
    same(r, c, 0, c, 1);
    assert.throws(() => r.selectNode(document.createElement('p')), 'InvalidNodeTypeError');
    assert.ok(r.commonAncestorContainer === c);
    r.setStart(a.firstChild, 1);
    assert.ok(r.commonAncestorContainer === d);
    d.remove();
});

test('a range in another tree moves both points', () => {
    const d = fixture('<p>one</p>');
    const other = document.createElement('section');
    other.textContent = 'detached';
    const r = document.createRange();
    r.selectNodeContents(d);
    r.setEnd(other.firstChild, 3);
    same(r, other.firstChild, 3, other.firstChild, 3);
    d.remove();
});

test('compareBoundaryPoints, comparePoint, isPointInRange, intersectsNode', () => {
    const d = fixture('<p>one</p><p>two</p><p>three</p>');
    const [a, b, c] = d.children;
    const r1 = document.createRange(), r2 = document.createRange();
    r1.setStart(a.firstChild, 1); r1.setEnd(b.firstChild, 2);
    r2.setStart(b.firstChild, 1); r2.setEnd(c.firstChild, 1);
    assert.equal(r1.compareBoundaryPoints(Range.START_TO_START, r2), -1);
    assert.equal(r1.compareBoundaryPoints(Range.START_TO_END, r2), 1, 'r1 end against r2 start');
    assert.equal(r1.compareBoundaryPoints(Range.END_TO_END, r2), -1);
    assert.equal(r1.compareBoundaryPoints(Range.END_TO_START, r2), -1, 'r1 start against r2 end');
    assert.equal(r1.compareBoundaryPoints(Range.START_TO_START, r1), 0);
    assert.throws(() => r1.compareBoundaryPoints(4, r2), 'NotSupportedError');
    const far = new Range();
    far.selectNodeContents(document.createElement('p'));
    assert.throws(() => r1.compareBoundaryPoints(0, far), 'WrongDocumentError');
    /* an ancestor container against a descendant point */
    const r3 = document.createRange();
    r3.setStart(d, 1); r3.setEnd(d, 2);
    assert.equal(r3.comparePoint(a.firstChild, 0), -1);
    assert.equal(r3.comparePoint(b.firstChild, 1), 0);
    assert.equal(r3.comparePoint(c, 0), 1);
    assert.equal(r3.comparePoint(d, 1), 0);
    assert.ok(r3.isPointInRange(b, 0));
    assert.ok(!r3.isPointInRange(a, 0));
    assert.ok(!r3.isPointInRange(document.createElement('p'), 0));
    assert.throws(() => r3.comparePoint(b, 5), 'IndexSizeError');
    assert.ok(r1.intersectsNode(a) && r1.intersectsNode(b) && !r1.intersectsNode(c));
    assert.ok(r1.intersectsNode(d));
    assert.ok(!r3.intersectsNode(a) && r3.intersectsNode(b.firstChild));
    d.remove();
});

test('boundary points follow insertions and removals', () => {
    const d = fixture('<i>a</i><i>b</i><i>c</i><i>d</i>');
    const [a, b, c, e] = d.children;
    const r = document.createRange();
    r.setStart(d, 1); r.setEnd(d, 3);
    d.insertBefore(document.createElement('u'), a);
    same(r, d, 2, d, 4, 'insert before the start');
    d.insertBefore(document.createElement('u'), b);
    same(r, d, 2, d, 5, 'insert at the start offset: the start stays');
    d.appendChild(document.createElement('u'));
    same(r, d, 2, d, 5, 'insert after the end');
    d.insertBefore(document.createElement('u'), e);
    same(r, d, 2, d, 5, 'insert at the end offset: the end stays');
    d.removeChild(d.firstChild);
    same(r, d, 1, d, 4, 'remove before the start');
    c.remove();
    same(r, d, 1, d, 3, 'remove inside');
    const frag = document.createDocumentFragment();
    frag.append(document.createElement('s'), document.createElement('s'));
    d.insertBefore(frag, d.firstChild);
    same(r, d, 3, d, 5, 'a fragment of two');
    d.remove();
});

test('removing a boundary container moves the point to its parent', () => {
    const d = fixture('<p>one <b>two</b> three</p><p>four</p>');
    const p = d.firstChild, b = p.querySelector('b');
    const r = document.createRange();
    r.setStart(b.firstChild, 1);
    r.setEnd(d.lastChild.firstChild, 2);
    b.remove();
    same(r, p, 1, d.lastChild.firstChild, 2, 'the start was inside the removed node');
    p.remove();
    same(r, d, 0, d.firstChild.firstChild, 2, 'then its parent went');
    d.textContent = '';
    same(r, d, 0, d, 0, 'everything removed');
    d.innerHTML = '<p>x</p>';
    same(r, d, 0, d, 0, 'innerHTML inserts after a collapsed point');
    d.remove();
});

test('character data edits move offsets', () => {
    const d = fixture('0123456789');
    const t = d.firstChild;
    const r = document.createRange();
    r.setStart(t, 3); r.setEnd(t, 7);
    t.insertData(1, 'ab');
    same(r, t, 5, t, 9, 'insert before');
    t.deleteData(0, 2);
    same(r, t, 3, t, 7, 'delete before');
    t.replaceData(4, 2, 'XYZ');
    same(r, t, 3, t, 8, 'replace inside');
    t.deleteData(2, 3);
    same(r, t, 2, t, 5, 'delete across the start');
    t.appendData('!!');
    same(r, t, 2, t, 5, 'append');
    t.data = 'new';
    same(r, t, 0, t, 0, 'a new value');
    t.data = 'new and more';
    r.setEnd(t, 3);
    t.data += 'x';
    same(r, t, 0, t, 3, 'data only appended');
    d.remove();
});

test('splitText moves points in the tail to the new node', () => {
    const d = fixture('hello world<b>x</b>');
    const t = d.firstChild;
    const r = document.createRange(), after = document.createRange();
    r.setStart(t, 2); r.setEnd(t, 8);
    after.setStart(d, 1); after.setEnd(d, 2);
    const tail = t.splitText(5);
    assert.equal(t.data, 'hello');
    assert.equal(tail.data, ' world');
    same(r, t, 2, tail, 3);
    same(after, d, 2, d, 3, 'points after the node move past the new one');
    d.remove();
});

test('toString, cloneContents, extractContents and deleteContents', () => {
    const d = fixture('<p>one <b>two</b> three</p><p>four <i>five</i></p><p>six</p>');
    const [p1, p2, p3] = d.children;
    const r = document.createRange();
    r.setStart(p1.firstChild, 2);
    r.setEnd(p2.querySelector('i').firstChild, 2);
    assert.equal(r.toString(), 'e two threefour fi');
    const c = r.cloneContents();
    const holder = document.createElement('div');
    holder.appendChild(c);
    assert.equal(holder.innerHTML, '<p>e <b>two</b> three</p><p>four <i>fi</i></p>');
    assert.equal(d.innerHTML, '<p>one <b>two</b> three</p><p>four <i>five</i></p><p>six</p>', 'cloning changes nothing');
    const x = r.extractContents();
    holder.textContent = '';
    holder.appendChild(x);
    assert.equal(holder.innerHTML, '<p>e <b>two</b> three</p><p>four <i>fi</i></p>');
    assert.equal(d.innerHTML, '<p>on</p><p><i>ve</i></p><p>six</p>');
    same(r, d, 1, d, 1, 'collapsed between the two halves');
    /* one text node */
    const r2 = document.createRange();
    r2.setStart(p3.firstChild, 1); r2.setEnd(p3.firstChild, 2);
    assert.equal(r2.toString(), 'i');
    r2.deleteContents();
    assert.equal(p3.textContent, 'sx');
    assert.ok(r2.collapsed);
    /* whole children */
    const r3 = document.createRange();
    r3.setStart(d, 0); r3.setEnd(d, 2);
    r3.deleteContents();
    assert.equal(d.innerHTML, '<p>sx</p>');
    same(r3, d, 0, d, 0);
    /* across text and elements */
    d.innerHTML = '<p>abc<b>def</b>ghi</p>';
    const r4 = document.createRange();
    r4.setStart(d.firstChild.firstChild, 1);
    r4.setEnd(d.firstChild.lastChild, 2);
    r4.deleteContents();
    assert.equal(d.innerHTML, '<p>ai</p>');
    same(r4, d.firstChild, 1, d.firstChild, 1, 'collapsed after the start node');
    assert.equal(document.createRange().cloneContents().childNodes.length, 0);
    d.remove();
});

test('insertNode and surroundContents', () => {
    const d = fixture('<p>hello world</p>');
    const t = d.firstChild.firstChild;
    const r = document.createRange();
    r.setStart(t, 5); r.setEnd(t, 5);
    const b = document.createElement('b');
    b.textContent = '!';
    r.insertNode(b);
    assert.equal(d.innerHTML, '<p>hello<b>!</b> world</p>');
    same(r, t, 5, d.firstChild, 2, 'a collapsed range ends after the inserted node');
    const r2 = document.createRange();
    r2.setStart(t, 1); r2.setEnd(t, 4);
    const em = document.createElement('em');
    em.textContent = 'old';
    r2.surroundContents(em);
    assert.equal(d.innerHTML, '<p>h<em>ell</em>o<b>!</b> world</p>');
    same(r2, d.firstChild, 1, d.firstChild, 2);
    const r3 = document.createRange();
    r3.setStart(d.firstChild.firstChild, 0);
    r3.setEnd(em.firstChild, 1);
    assert.throws(() => r3.surroundContents(document.createElement('u')), 'InvalidStateError');
    assert.throws(() => r2.surroundContents(document.createDocumentFragment()), 'InvalidNodeTypeError');
    const c = document.createComment('c');
    d.appendChild(c);
    const r4 = document.createRange();
    r4.setStart(c, 0);
    assert.throws(() => r4.insertNode(document.createElement('u')), 'HierarchyRequestError');
    const f = document.createRange().createContextualFragment('<i>x</i>y');
    assert.equal(f.childNodes.length, 2);
    d.remove();
});

test('cloneRange is independent; a range in a removed subtree moves out', () => {
    const d = fixture('<p>abc</p>');
    const r = document.createRange();
    r.selectNodeContents(d.firstChild);
    const c = r.cloneRange();
    c.collapse(true);
    same(r, d.firstChild, 0, d.firstChild, 1);
    same(c, d.firstChild, 0, d.firstChild, 0);
    const p = d.firstChild;
    const inner = document.createRange();
    inner.setStart(p.firstChild, 1); inner.setEnd(p.firstChild, 2);
    const at = Array.prototype.indexOf.call(document.body.childNodes, d);
    d.remove();
    same(inner, document.body, at, document.body, at, 'where the subtree was');
    /* a range made in a detached tree stays in it */
    const det = document.createRange();
    det.setStart(p.firstChild, 1); det.setEnd(p.firstChild, 2);
    assert.equal(det.toString(), 'b');
    assert.ok(det.commonAncestorContainer === p.firstChild);
});

test('range rectangles', () => {
    const d = fixture('<p style="margin:0;width:200px">text <b>bold</b></p>');
    const r = document.createRange();
    r.selectNodeContents(d.firstChild);
    /* this runner has no layout: sizes are checked in pages/input.html */
    const box = r.getBoundingClientRect();
    assert.ok(box instanceof DOMRect && box.width >= 0);
    assert.ok(r.getClientRects().length >= 0);
    r.collapse(true);
    assert.equal(r.getClientRects().length, 0);
    assert.equal(r.getBoundingClientRect().width, 0);
    d.remove();
});

test('StaticRange', () => {
    const d = fixture('<p>abc</p>');
    const s = new StaticRange({ startContainer: d.firstChild.firstChild, startOffset: 1, endContainer: d, endOffset: 1 });
    assert.equal(s.startOffset, 1);
    assert.ok(!s.collapsed);
    assert.ok(s instanceof AbstractRange);
    assert.throws(() => new StaticRange({}), TypeError);
    d.remove();
});

promise_test('Selection: ranges, direction, extend and selectionchange', async () => {
    const d = fixture('<p>one two</p><p>three</p>');
    const t1 = d.firstChild.firstChild, t2 = d.lastChild.firstChild;
    const sel = getSelection();
    assert.ok(sel === document.getSelection());
    sel.removeAllRanges();
    await delay(10);
    let changes = 0;
    const on = () => changes++;
    document.addEventListener('selectionchange', on);
    assert.equal(sel.type, 'None');
    assert.equal(sel.rangeCount, 0);
    assert.equal(sel.anchorNode, null);
    sel.collapse(t1, 2);
    assert.equal(sel.type, 'Caret');
    assert.ok(sel.isCollapsed);
    sel.extend(t2, 3);
    assert.equal(sel.type, 'Range');
    assert.equal(sel.direction, 'forward');
    assert.equal(sel.toString(), 'e twothr');
    assert.ok(sel.anchorNode === t1 && sel.anchorOffset === 2 && sel.focusNode === t2 && sel.focusOffset === 3);
    sel.extend(t1, 0);
    assert.equal(sel.direction, 'backward');
    assert.ok(sel.anchorNode === t1 && sel.anchorOffset === 2 && sel.focusNode === t1 && sel.focusOffset === 0);
    assert.equal(sel.getRangeAt(0).startOffset, 0);
    sel.setBaseAndExtent(t2, 4, t1, 4);
    assert.equal(sel.toString(), 'twothre');
    assert.ok(sel.anchorNode === t2 && sel.focusNode === t1);
    sel.collapseToStart();
    assert.ok(sel.anchorNode === t1 && sel.anchorOffset === 4 && sel.isCollapsed);
    sel.selectAllChildren(d);
    assert.equal(sel.toString(), 'one twothree');
    assert.ok(sel.containsNode(d.firstChild) && sel.containsNode(t2));
    assert.ok(!sel.containsNode(d) && sel.containsNode(d, true));
    const r = document.createRange();
    r.selectNode(d.lastChild);
    sel.addRange(r);
    assert.equal(sel.rangeCount, 1, 'a second range is ignored');
    assert.ok(sel.getRangeAt(0) !== r);
    sel.removeAllRanges();
    sel.addRange(r);
    assert.ok(sel.getRangeAt(0) === r, 'the selection holds the range itself');
    r.setStart(t1, 0);
    assert.equal(sel.toString(), 'one twothree', 'changing the range changes the selection');
    assert.throws(() => sel.getRangeAt(1), 'IndexSizeError');
    sel.deleteFromDocument();
    assert.equal(d.textContent, '');
    sel.empty();
    assert.throws(() => sel.collapseToEnd(), 'InvalidStateError');
    await delay(10);
    document.removeEventListener('selectionchange', on);
    assert.ok(changes >= 1, 'selectionchange fired');
    d.remove();
});
