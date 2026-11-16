// Shadow DOM: attachShadow, slots, event retargeting, declarative roots.

test('attachShadow and modes', () => {
    const host = document.createElement('div');
    const root = host.attachShadow({ mode: 'open' });
    assert.ok(root instanceof ShadowRoot);
    assert.equal(host.shadowRoot, root);
    assert.equal(root.host, host);
    assert.equal(root.mode, 'open');
    assert.throws(() => host.attachShadow({ mode: 'open' }), 'NotSupportedError');
    const closed = document.createElement('span').attachShadow({ mode: 'closed' });
    assert.equal(closed.host.shadowRoot, null);
    assert.throws(() => document.createElement('img').attachShadow({ mode: 'open' }), 'NotSupportedError');
    assert.throws(() => document.createElement('div').attachShadow({}), TypeError);
});

test('shadow tree is separate from the light tree', () => {
    const host = document.createElement('div');
    document.body.appendChild(host);
    const root = host.attachShadow({ mode: 'open' });
    root.innerHTML = '<p id=inner>shadow</p>';
    assert.equal(document.getElementById('inner'), null);
    assert.equal(root.getElementById('inner').textContent, 'shadow');
    assert.equal(root.querySelector('p').getRootNode(), root);
    assert.equal(root.querySelector('p').getRootNode({ composed: true }), document);
    assert.ok(root.querySelector('p').isConnected);
    assert.equal(host.textContent, '');
    host.remove();
});

test('slots and assignment', () => {
    const host = document.createElement('div');
    host.innerHTML = '<span slot=a>A</span><b>default</b>';
    const root = host.attachShadow({ mode: 'open' });
    root.innerHTML = '<slot name=a></slot><slot></slot>';
    const [sa, sd] = root.querySelectorAll('slot');
    assert.equal(sa.assignedNodes().length, 1);
    assert.equal(sa.assignedNodes()[0].textContent, 'A');
    assert.equal(sd.assignedElements()[0].localName, 'b');
    assert.equal(host.firstChild.assignedSlot, sa);
    host.firstChild.slot = 'none';
    assert.equal(sa.assignedNodes().length, 0);
});

promise_test('slotchange fires', async () => {
    const host = document.createElement('div');
    document.body.appendChild(host);
    const root = host.attachShadow({ mode: 'open' });
    root.innerHTML = '<slot></slot>';
    const slot = root.querySelector('slot');
    const p = once(slot, 'slotchange');
    host.appendChild(document.createElement('i'));
    await p;
    host.remove();
});

test('events are retargeted across the shadow boundary', () => {
    const host = document.createElement('div');
    document.body.appendChild(host);
    const root = host.attachShadow({ mode: 'open' });
    root.innerHTML = '<button>b</button>';
    const btn = root.querySelector('button');
    const seen = [];
    document.body.addEventListener('ev', (e) => seen.push(['body', e.target === host]));
    root.addEventListener('ev', (e) => seen.push(['root', e.target === btn]));
    btn.dispatchEvent(new Event('ev', { bubbles: true, composed: true }));
    assert.deepEqual(seen, [['root', true], ['body', true]]);
    seen.length = 0;
    btn.dispatchEvent(new Event('ev', { bubbles: true }));
    assert.deepEqual(seen, [['root', true]], 'non-composed events stop at the root');
    let path = null;
    host.addEventListener('ev2', (e) => { path = e.composedPath(); });
    btn.dispatchEvent(new Event('ev2', { bubbles: true, composed: true }));
    assert.equal(path[0], btn);
    assert.ok(path.includes(root));
    host.remove();
});

test('closed roots hide their nodes from composedPath outside', () => {
    const host = document.createElement('div');
    document.body.appendChild(host);
    const root = host.attachShadow({ mode: 'closed' });
    root.innerHTML = '<i></i>';
    let outside = null;
    document.body.addEventListener('c', (e) => { outside = e.composedPath(); }, { once: true });
    root.firstChild.dispatchEvent(new Event('c', { bubbles: true, composed: true }));
    assert.equal(outside[0], host);
    host.remove();
});

test('declarative shadow roots through setHTMLUnsafe-style parsing', () => {
    const d = document.createElement('div');
    d.innerHTML = '<div id=h><template shadowrootmode=open><b>in shadow</b></template>light</div>';
    // innerHTML does not attach declarative roots (as in browsers)
    assert.equal(d.firstChild.shadowRoot, null);
    assert.equal(d.querySelector('template').localName, 'template');
});

test('setHTMLUnsafe attaches declarative shadow roots', () => {
    const d = document.createElement('div');
    d.setHTMLUnsafe('<div id=h><template shadowrootmode=open shadowrootdelegatesfocus shadowrootclonable>' +
        '<b>in shadow</b><slot></slot></template>light</div>');
    const h = d.firstChild;
    assert.ok(h.shadowRoot);
    assert.equal(h.shadowRoot.querySelector('b').textContent, 'in shadow');
    assert.equal(h.shadowRoot.delegatesFocus, true);
    assert.equal(h.shadowRoot.clonable, true);
    assert.equal(h.querySelector('template'), null);
    assert.equal(h.textContent, 'light');
    assert.equal(h.shadowRoot.querySelector('slot').assignedNodes()[0].data, 'light');
    /* closed: no shadowRoot getter; a second template stays a template */
    d.setHTMLUnsafe('<span><template shadowrootmode=closed><i></i></template>' +
        '<template shadowrootmode=open><u></u></template></span>');
    assert.equal(d.firstChild.shadowRoot, null);
    assert.equal(d.firstChild.querySelectorAll('template').length, 1);
    /* not a valid host, or an invalid mode: an ordinary template */
    d.setHTMLUnsafe('<ul><template shadowrootmode=open></template></ul><p><template shadowrootmode=bogus></template></p>');
    assert.equal(d.querySelectorAll('template').length, 2);
    /* attachShadow on a declarative root empties and returns it */
    d.setHTMLUnsafe('<div><template shadowrootmode=open><b>x</b></template></div>');
    const r = d.firstChild.shadowRoot;
    assert.equal(d.firstChild.attachShadow({ mode: 'open' }), r);
    assert.equal(r.childNodes.length, 0);
});

test('Document.parseHTMLUnsafe and DOMParser', () => {
    const html = '<div><template shadowrootmode=open><p>s</p></template></div>';
    const a = Document.parseHTMLUnsafe(html);
    assert.ok(a.querySelector('div').shadowRoot);
    const b = new DOMParser().parseFromString(html, 'text/html');
    assert.equal(b.querySelector('div').shadowRoot, null);
});

test('assignedNodes with flatten and fallback', () => {
    const outer = document.createElement('div');
    outer.innerHTML = '<span>A</span>';
    const r1 = outer.attachShadow({ mode: 'open' });
    r1.innerHTML = '<div id=inner><slot id=s1></slot></div>';
    const inner = r1.getElementById('inner');
    const r2 = inner.attachShadow({ mode: 'open' });
    r2.innerHTML = '<slot id=s2>fallback</slot>';
    const s2 = r2.getElementById('s2');
    assert.equal(s2.assignedNodes().length, 1);
    assert.equal(s2.assignedNodes()[0].localName, 'slot');
    const flat = s2.assignedNodes({ flatten: true });
    assert.equal(flat.length, 1);
    assert.equal(flat[0].textContent, 'A');
    outer.firstChild.remove();
    const fb = s2.assignedNodes({ flatten: true });
    assert.equal(fb.length, 0, 's1 has no fallback content');
    inner.removeChild(r1.getElementById('s1'));
    assert.equal(s2.assignedNodes({ flatten: true })[0].data, 'fallback');
    assert.equal(s2.assignedElements({ flatten: true }).length, 0);
});

test('manual slot assignment', () => {
    const host = document.createElement('div');
    host.innerHTML = '<b>1</b><i>2</i>';
    const root = host.attachShadow({ mode: 'open', slotAssignment: 'manual' });
    root.innerHTML = '<slot id=a></slot><slot id=b name=x></slot>';
    const [a, b] = root.querySelectorAll('slot');
    const [one, two] = host.children;
    assert.equal(root.slotAssignment, 'manual');
    assert.equal(a.assignedNodes().length, 0, 'nothing is assigned by name');
    a.assign(two, one);
    assert.deepEqual(a.assignedNodes().map((n) => n.textContent), ['2', '1']);
    assert.equal(one.assignedSlot, a);
    b.assign(one);
    assert.deepEqual(a.assignedNodes().map((n) => n.textContent), ['2']);
    assert.equal(one.assignedSlot, b);
    b.assign();
    assert.equal(one.assignedSlot, null);
    /* a node that is not a child of the host is remembered, not assigned */
    const stray = document.createElement('u');
    a.assign(stray);
    assert.equal(a.assignedNodes().length, 0);
    host.appendChild(stray);
    assert.equal(a.assignedNodes()[0], stray);
    assert.throws(() => a.assign({}), TypeError);
});

test('activeElement and focus retarget across shadow trees', () => {
    const host = document.createElement('div');
    document.body.appendChild(host);
    const root = host.attachShadow({ mode: 'open', delegatesFocus: true });
    root.innerHTML = '<span>text</span><input id=in>';
    const input = root.getElementById('in');
    host.focus();
    assert.equal(root.activeElement, input, 'delegatesFocus focuses the first focusable element');
    assert.equal(document.activeElement, host, 'the document sees the host');
    input.blur();
    assert.equal(root.activeElement, null);
    const plain = document.createElement('div');
    document.body.appendChild(plain);
    const r2 = plain.attachShadow({ mode: 'open' });
    r2.innerHTML = '<input>';
    plain.focus();
    assert.equal(r2.activeElement, null, 'no delegation without delegatesFocus');
    host.remove();
    plain.remove();
});
