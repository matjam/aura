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
});
