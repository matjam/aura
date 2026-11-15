// Live collections, attributes, DOMTokenList, dataset, inline style.

test('childNodes and children are live and cached', () => {
    const d = document.createElement('div');
    const kids = d.childNodes, els = d.children;
    assert.equal(kids, d.childNodes);
    assert.equal(els, d.children);
    d.append('t', document.createElement('a'), document.createElement('b'));
    assert.equal(kids.length, 3);
    assert.equal(els.length, 2);
    assert.equal(els[1].localName, 'b');
    assert.equal(kids.item(0).nodeType, 3);
    assert.equal(kids[5], undefined);
    assert.equal(kids.item(5), null);
    assert.deepEqual(Object.keys(els), ['0', '1']);
    assert.ok(kids instanceof NodeList);
    assert.ok(els instanceof HTMLCollection);
    d.firstElementChild.remove();
    assert.equal(els.length, 1);
    kids[0] = null;
    assert.equal(kids[0].nodeType, 3, 'indices are read only');
});

test('getElementsByTagName and ClassName are live', () => {
    const d = document.createElement('div');
    document.body.appendChild(d);
    const ps = document.getElementsByTagName('p');
    const xs = document.getElementsByClassName('x');
    const n0 = ps.length;
    d.innerHTML = '<p class=x></p><p></p><span class="y x"></span>';
    assert.equal(ps.length, n0 + 2);
    assert.equal(xs.length, 2);
    d.lastChild.className = 'y';
    assert.equal(xs.length, 1);
    d.remove();
    assert.equal(ps.length, n0);
    assert.equal(document.getElementsByTagName('*').length, document.all.length);
});

test('HTMLCollection namedItem and named access', () => {
    const f = document.createElement('div');
    f.innerHTML = '<a id=one></a><a name=two></a>';
    assert.equal(f.children.namedItem('one').id, 'one');
    assert.equal(f.children.two.getAttribute('name'), 'two');
    assert.equal(f.children.namedItem('none'), null);
});

test('querySelectorAll is static', () => {
    const d = document.createElement('div');
    d.innerHTML = '<i></i><i></i>';
    const l = d.querySelectorAll('i');
    d.appendChild(document.createElement('i'));
    assert.equal(l.length, 2);
    let n = 0;
    l.forEach(() => n++);
    assert.equal(n, 2);
    assert.equal([...l].length, 2);
});

test('attributes API', () => {
    const e = document.createElement('div');
    e.setAttribute('Data-X', '1');
    assert.equal(e.getAttribute('data-x'), '1');
    assert.ok(e.hasAttribute('DATA-X'));
    assert.deepEqual(e.getAttributeNames(), ['data-x']);
    assert.equal(e.toggleAttribute('hidden'), true);
    assert.equal(e.toggleAttribute('hidden'), false);
    assert.equal(e.toggleAttribute('hidden', false), false);
    e.removeAttribute('data-x');
    assert.ok(!e.hasAttributes());
    assert.throws(() => e.setAttribute('a b', ''), 'InvalidCharacterError');
    e.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', '#a');
    assert.equal(e.getAttributeNS('http://www.w3.org/1999/xlink', 'href'), '#a');
    e.id = 'q';
    e.className = 'a b';
    assert.equal(e.getAttribute('id'), 'q');
    assert.equal(e.attributes.length, 3);
    assert.equal(e.attributes.id.value, 'q');
    assert.equal(e.attributes[0].ownerElement, e);
    const a = document.createAttribute('title');
    a.value = 't';
    e.setAttributeNode(a);
    assert.equal(e.title, 't');
    assert.equal(e.getAttributeNode('title'), a);
    e.removeAttributeNode(a);
    assert.equal(e.title, '');
    assert.equal(a.value, 't');
});

test('classList', () => {
    const e = document.createElement('div');
    const l = e.classList;
    assert.equal(l, e.classList);
    l.add('a', 'b');
    assert.equal(e.className, 'a b');
    l.add('a');
    assert.equal(l.length, 2);
    assert.ok(l.contains('b'));
    l.remove('a');
    assert.equal(e.className, 'b');
    assert.equal(l.toggle('c'), true);
    assert.equal(l.toggle('c'), false);
    assert.equal(l.toggle('d', true), true);
    assert.ok(l.replace('b', 'e'));
    assert.equal(l.value, 'e d');
    assert.equal(l[0], 'e');
    assert.throws(() => l.add(''), 'SyntaxError');
    assert.throws(() => l.add('a b'), 'InvalidCharacterError');
    assert.deepEqual([...l], ['e', 'd']);
    const a = document.createElement('link');
    assert.ok(a.relList.supports('stylesheet'));
});

test('dataset', () => {
    const e = document.createElement('div');
    e.dataset.fooBar = 'x';
    assert.equal(e.getAttribute('data-foo-bar'), 'x');
    e.setAttribute('data-a-b-c', 'y');
    assert.equal(e.dataset.aBC, 'y');
    assert.ok('fooBar' in e.dataset);
    assert.deepEqual(Object.keys(e.dataset), ['fooBar', 'aBC']);
    delete e.dataset.fooBar;
    assert.ok(!e.hasAttribute('data-foo-bar'));
    assert.equal(e.dataset.missing, undefined);
});

test('inline style', () => {
    const e = document.createElement('div');
    e.style.color = 'red';
    // src/css serializes the specified color as rgb(255, 0, 0); browsers
    // keep "red". Known difference, reported to src/css.
    assert.ok(/^color: (red|rgb\(255, 0, 0\));$/.test(e.getAttribute('style')), e.getAttribute('style'));
    assert.equal(e.style.color, e.style.getPropertyValue('color'));
    e.style.marginTop = '10px';
    assert.equal(e.style.marginTop, '10px');
    assert.equal(e.style.getPropertyValue('margin-top'), '10px');
    e.style.setProperty('padding-left', '3px', 'important');
    assert.equal(e.style.getPropertyPriority('padding-left'), 'important');
    e.style.width = 'banana';
    assert.equal(e.style.width, '', 'invalid values are ignored');
    e.style.color = '';
    assert.equal(e.style.color, '');
    e.style.cssText = 'display: none';
    assert.equal(e.style.display, 'none');
    assert.equal(e.style.length, 1);
    assert.equal(e.style.item(0), 'display');
    e.style.removeProperty('display');
    assert.equal(e.style.cssText, '');
    e.style.cssFloat = 'left';
    assert.equal(e.style.float, 'left');
    e.style.setProperty('--my-var', '5');
    assert.equal(e.style.getPropertyValue('--my-var'), '5');
    e.style.border = '1px solid red';
    assert.equal(e.style.borderTopWidth, '1px');
});

test('CSS.supports and escape', () => {
    assert.ok(CSS.supports('display', 'block'));
    assert.ok(!CSS.supports('display', 'banana'));
    assert.ok(CSS.supports('(color: red)'));
    assert.equal(CSS.escape('a b'), 'a\\ b');
    assert.equal(CSS.escape('1a'), '\\31 a');
});

test('getComputedStyle without layout returns a declaration', () => {
    const cs = getComputedStyle(document.body);
    assert.ok(cs instanceof CSSStyleDeclaration);
    assert.equal(typeof cs.display, 'string');
    assert.throws(() => cs.setProperty('color', 'red'), 'NoModificationAllowedError');
    assert.throws(() => getComputedStyle({}), TypeError);
});
