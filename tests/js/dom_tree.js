// Node, ParentNode/ChildNode mixins, Document, Element: tree operations.

test('globals', () => {
    assert.equal(window, globalThis);
    assert.equal(self, window);
    assert.equal(window.window, window);
    assert.equal(document.defaultView, window);
    assert.ok(window instanceof Window);
    assert.ok(document instanceof Document);
    assert.ok(document instanceof Node);
    assert.ok(document instanceof EventTarget);
    assert.equal(Object.prototype.toString.call(document.body), '[object HTMLBodyElement]');
});

test('createElement and wrappers are cached', () => {
    const d = document.createElement('div');
    assert.ok(d instanceof HTMLDivElement);
    assert.ok(d instanceof HTMLElement);
    assert.equal(d.tagName, 'DIV');
    assert.equal(d.localName, 'div');
    assert.equal(d.namespaceURI, 'http://www.w3.org/1999/xhtml');
    document.body.appendChild(d);
    assert.equal(document.body.lastChild, d);
    d.expando = 42;
    assert.equal(document.body.lastChild.expando, 42);
    d.remove();
    assert.equal(d.parentNode, null);
});

test('createElement validates names', () => {
    assert.throws(() => document.createElement(''), 'InvalidCharacterError');
    assert.throws(() => document.createElement('a b'), 'InvalidCharacterError');
    assert.throws(() => document.createElement('1x'), 'InvalidCharacterError');
    assert.equal(document.createElement('DIV').localName, 'div');
    assert.ok(document.createElement('foo') instanceof HTMLUnknownElement);
    assert.ok(document.createElement('foo-bar') instanceof HTMLElement);
});

test('append, prepend, before, after, replaceWith', () => {
    const p = document.createElement('p');
    p.append('b', document.createElement('i'));
    p.prepend('a');
    assert.equal(p.childNodes.length, 3);
    assert.equal(p.textContent, 'ab');
    const i = p.querySelector('i');
    i.before('x');
    i.after('y', 'z');
    assert.equal(p.textContent, 'abxyz');
    i.replaceWith(document.createElement('b'));
    assert.equal(p.querySelector('i'), null);
    assert.ok(p.querySelector('b'));
    p.replaceChildren();
    assert.equal(p.firstChild, null);
});

test('insertBefore and hierarchy errors', () => {
    const a = document.createElement('div');
    const b = document.createElement('div');
    a.appendChild(b);
    assert.throws(() => b.appendChild(a), 'HierarchyRequestError');
    assert.throws(() => a.insertBefore(document.createElement('p'), document.createElement('p')), 'NotFoundError');
    assert.throws(() => document.appendChild(document.createElement('div')), 'HierarchyRequestError');
    assert.throws(() => a.appendChild(document), 'HierarchyRequestError');
    assert.throws(() => a.appendChild('text'), TypeError);
    assert.throws(() => a.appendChild(null), TypeError);
});

test('fragments move their children', () => {
    const f = document.createDocumentFragment();
    f.appendChild(document.createElement('a'));
    f.appendChild(document.createElement('b'));
    const d = document.createElement('div');
    d.appendChild(f);
    assert.equal(f.childNodes.length, 0);
    assert.equal(d.children.length, 2);
});

test('textContent, nodeValue, data', () => {
    const d = document.createElement('div');
    d.innerHTML = '<b>one</b> two <!--c-->';
    assert.equal(d.textContent, 'one two ');
    d.textContent = 'x';
    assert.equal(d.childNodes.length, 1);
    assert.equal(d.firstChild.nodeType, Node.TEXT_NODE);
    const t = d.firstChild;
    t.data = 'hello';
    assert.equal(d.textContent, 'hello');
    assert.equal(t.nodeValue, 'hello');
    assert.equal(t.length, 5);
    t.appendData('!');
    t.insertData(0, '>');
    t.deleteData(1, 1);
    assert.equal(t.data, '>ello!');
    assert.equal(t.substringData(1, 3), 'ell');
    assert.throws(() => t.substringData(100, 1), 'IndexSizeError');
    const t2 = t.splitText(3);
    assert.equal(t.data, '>el');
    assert.equal(t2.data, 'lo!');
    assert.equal(t.wholeText, '>ello!');
    d.normalize();
    assert.equal(d.childNodes.length, 1);
    assert.equal(d.firstChild.data, '>ello!');
});

test('cloneNode, isEqualNode, contains, compareDocumentPosition', () => {
    const d = document.createElement('div');
    d.innerHTML = '<p id=a class="x y">t<i>u</i></p>';
    const c = d.cloneNode(true);
    assert.ok(c.isEqualNode(d));
    assert.ok(!c.isSameNode(d));
    assert.equal(c.innerHTML, d.innerHTML);
    const shallow = d.cloneNode();
    assert.equal(shallow.childNodes.length, 0);
    assert.ok(d.contains(d.querySelector('i')));
    assert.ok(!d.contains(c));
    const p = d.firstChild, i = d.querySelector('i');
    assert.equal(p.compareDocumentPosition(i), Node.DOCUMENT_POSITION_CONTAINED_BY | Node.DOCUMENT_POSITION_FOLLOWING);
    assert.equal(i.compareDocumentPosition(p), Node.DOCUMENT_POSITION_CONTAINS | Node.DOCUMENT_POSITION_PRECEDING);
    assert.ok(d.compareDocumentPosition(c) & Node.DOCUMENT_POSITION_DISCONNECTED);
});

test('innerHTML, outerHTML, insertAdjacentHTML', () => {
    const d = document.createElement('div');
    d.innerHTML = '<span>a</span><br>&amp;<img alt="x&quot;y">';
    assert.equal(d.innerHTML, '<span>a</span><br>&amp;<img alt="x&quot;y">');
    assert.equal(d.firstChild.outerHTML, '<span>a</span>');
    d.insertAdjacentHTML('afterbegin', '<em>1</em>');
    d.insertAdjacentHTML('beforeend', '<em>2</em>');
    assert.equal(d.firstChild.textContent, '1');
    assert.equal(d.lastChild.textContent, '2');
    document.body.appendChild(d);
    d.firstChild.outerHTML = '<u>o</u>';
    assert.equal(d.firstChild.localName, 'u');
    d.remove();
    const t = document.createElement('table');
    t.innerHTML = '<tr><td>x</td></tr>';
    assert.equal(t.rows.length, 1);
    assert.equal(t.firstChild.localName, 'tbody');
});

test('getElementById, querySelector, matches, closest', () => {
    const d = document.createElement('div');
    d.innerHTML = '<ul><li id=one class=a>1</li><li class="a b">2</li></ul>';
    document.body.appendChild(d);
    assert.equal(document.getElementById('one').textContent, '1');
    assert.equal(d.querySelector('.b').textContent, '2');
    assert.equal(d.querySelectorAll('li.a').length, 2);
    assert.ok(d.querySelector('#one').matches('ul > li:first-child'));
    assert.equal(d.querySelector('.b').closest('ul').localName, 'ul');
    assert.equal(d.querySelector('.b').closest('table'), null);
    assert.throws(() => d.querySelector('!!'), 'SyntaxError');
    d.remove();
    assert.equal(document.getElementById('one'), null);
});

test('document members', () => {
    assert.equal(document.documentElement.localName, 'html');
    assert.equal(document.head.localName, 'head');
    assert.equal(document.body.localName, 'body');
    assert.equal(document.title, 'test');
    document.title = 'changed';
    assert.equal(document.title, 'changed');
    assert.equal(document.querySelector('title').textContent, 'changed');
    assert.equal(document.compatMode, 'CSS1Compat');
    assert.equal(document.doctype.name, 'html');
    assert.equal(document.characterSet, 'UTF-8');
    assert.equal(document.nodeType, 9);
    assert.ok(document.URL.startsWith('file:'));
    assert.equal(document.readyState, 'interactive');
});

test('createTextNode, createComment, constructors', () => {
    const t = new Text('abc');
    assert.equal(t.data, 'abc');
    assert.equal(t.ownerDocument, document);
    const c = new Comment('x');
    assert.equal(c.nodeType, 8);
    const f = new DocumentFragment();
    assert.equal(f.nodeType, 11);
    assert.throws(() => new Node(), TypeError);
    assert.throws(() => new Element(), TypeError);
    assert.throws(() => new HTMLDivElement(), TypeError);
});

test('importNode and adoptNode', () => {
    const d = document.createElement('div');
    d.innerHTML = '<i>x</i>';
    const c = document.importNode(d, true);
    assert.equal(c.innerHTML, '<i>x</i>');
    const a = document.adoptNode(d.firstChild);
    assert.equal(a.parentNode, null);
    assert.throws(() => document.adoptNode(document), 'NotSupportedError');
});

test('TreeWalker and NodeIterator', () => {
    const d = document.createElement('div');
    d.innerHTML = '<a>1</a><b><c>2</c></b>';
    const w = document.createTreeWalker(d, NodeFilter.SHOW_ELEMENT);
    const names = [];
    while (w.nextNode()) names.push(w.currentNode.localName);
    assert.deepEqual(names, ['a', 'b', 'c']);
    const it = document.createNodeIterator(d, NodeFilter.SHOW_TEXT);
    const texts = [];
    for (let n = it.nextNode(); n; n = it.nextNode()) texts.push(n.data);
    assert.deepEqual(texts, ['1', '2']);
    const w2 = document.createTreeWalker(d, NodeFilter.SHOW_ELEMENT, (n) => (n.localName === 'b' ? NodeFilter.FILTER_SKIP : NodeFilter.FILTER_ACCEPT));
    const n2 = [];
    while (w2.nextNode()) n2.push(w2.currentNode.localName);
    assert.deepEqual(n2, ['a', 'c']);
});

test('Range basics', () => {
    const d = document.createElement('div');
    d.textContent = 'hello world';
    const r = document.createRange();
    r.setStart(d.firstChild, 0);
    r.setEnd(d.firstChild, 5);
    assert.equal(r.toString(), 'hello');
    assert.ok(!r.collapsed);
    r.deleteContents();
    assert.equal(d.textContent, ' world');
    const r2 = document.createRange();
    r2.selectNodeContents(d);
    assert.equal(r2.endOffset, 1);
    const f = r2.createContextualFragment('<b>x</b>');
    assert.equal(f.firstChild.localName, 'b');
});

test('removing nodes during iteration is safe', () => {
    const d = document.createElement('div');
    for (let i = 0; i < 50; i++) d.appendChild(document.createElement('span'));
    const live = d.children;
    let n = 0;
    for (const c of d.childNodes) { c.remove(); n++; }
    assert.equal(n, 50);
    assert.equal(live.length, 0);
    assert.equal(d.firstChild, null);
});
