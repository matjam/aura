// HTML element interfaces: reflected attributes, form controls, tables.

test('element interfaces by tag', () => {
    const map = { a: HTMLAnchorElement, img: HTMLImageElement, input: HTMLInputElement, table: HTMLTableElement,
        td: HTMLTableCellElement, th: HTMLTableCellElement, h1: HTMLHeadingElement, ul: HTMLUListElement,
        video: HTMLVideoElement, audio: HTMLAudioElement, template: HTMLTemplateElement, slot: HTMLSlotElement,
        q: HTMLQuoteElement, blockquote: HTMLQuoteElement, ins: HTMLModElement, del: HTMLModElement,
        span: HTMLSpanElement, unknownthing: HTMLUnknownElement, section: HTMLElement, b: HTMLElement };
    for (const [t, C] of Object.entries(map)) assert.equal(Object.getPrototypeOf(document.createElement(t)), C.prototype, t);
    assert.ok(document.createElement('video') instanceof HTMLMediaElement);
});

test('reflected string, boolean, URL attributes', () => {
    const a = document.createElement('a');
    a.href = 'x/y?z#h';
    assert.equal(a.getAttribute('href'), 'x/y?z#h');
    assert.ok(a.href.startsWith('file:///') && a.href.endsWith('/x/y?z#h'));
    assert.equal(a.hash, '#h');
    assert.equal(a.search, '?z');
    a.hash = 'k';
    assert.ok(a.href.endsWith('#k'));
    a.target = '_blank';
    assert.equal(a.getAttribute('target'), '_blank');
    const i = document.createElement('input');
    i.disabled = true;
    assert.equal(i.getAttribute('disabled'), '');
    i.disabled = false;
    assert.ok(!i.hasAttribute('disabled'));
    assert.equal(document.createElement('img').src, '');
    const d = document.createElement('div');
    d.hidden = true;
    d.title = 'tt';
    d.lang = 'fr';
    assert.equal(d.outerHTML, '<div hidden="" title="tt" lang="fr"></div>');
    i.htmlFor = 'x';
    const l = document.createElement('label');
    l.htmlFor = 'x';
    assert.equal(l.getAttribute('for'), 'x');
});

test('reflected numbers and enumerations', () => {
    const td = document.createElement('td');
    assert.equal(td.colSpan, 1);
    td.colSpan = 3;
    assert.equal(td.getAttribute('colspan'), '3');
    td.setAttribute('colspan', 'abc');
    assert.equal(td.colSpan, 1);
    const i = document.createElement('input');
    assert.equal(i.maxLength, -1);
    assert.equal(i.size, 20);
    assert.throws(() => { i.maxLength = -5; }, 'IndexSizeError');
    const f = document.createElement('form');
    assert.equal(f.method, 'get');
    f.method = 'POST';
    assert.equal(f.method, 'post');
    f.setAttribute('method', 'bogus');
    assert.equal(f.method, 'get');
    const c = document.createElement('canvas');
    assert.equal(c.width, 300);
    assert.equal(c.getContext('webgl'), null);
    assert.equal(c.getContext('2d').canvas, c); /* the 2D context exists: tests/canvas covers it */
    const b = document.createElement('button');
    assert.equal(b.type, 'submit');
    b.type = 'reset';
    assert.equal(b.type, 'reset');
    const div = document.createElement('div');
    assert.equal(div.tabIndex, -1);
    assert.equal(document.createElement('a').tabIndex, 0);
});

test('input value, checked, type', () => {
    const i = document.createElement('input');
    assert.equal(i.type, 'text');
    i.type = 'bogus';
    assert.equal(i.type, 'text');
    i.setAttribute('value', 'def');
    assert.equal(i.value, 'def');
    i.value = 'typed';
    assert.equal(i.value, 'typed');
    assert.equal(i.getAttribute('value'), 'def');
    assert.equal(i.defaultValue, 'def');
    i.value = 'a\nb';
    assert.equal(i.value, 'ab', 'line breaks are stripped');
    i.type = 'number';
    i.value = '12.5';
    assert.equal(i.valueAsNumber, 12.5);
    i.value = 'abc';
    assert.equal(i.value, '');
    i.type = 'checkbox';
    assert.equal(i.value, 'def', 'value mode default/on reads the attribute');
    i.removeAttribute('value');
    assert.equal(i.value, 'on');
    assert.ok(!i.checked);
    i.defaultChecked = true;
    assert.ok(i.checked);
    i.checked = false;
    assert.ok(!i.checked);
    assert.ok(i.hasAttribute('checked'));
});

test('select and option', () => {
    const s = document.createElement('select');
    s.innerHTML = '<option>a</option><option value=bv selected>b</option><option>c</option>';
    assert.equal(s.selectedIndex, 1);
    assert.equal(s.value, 'bv');
    assert.equal(s.options.length, 3);
    s.value = 'c';
    assert.equal(s.selectedIndex, 2);
    s.selectedIndex = 0;
    assert.equal(s.value, 'a');
    assert.equal(s.options[0].selected, true);
    s.add(new Option('d', 'dv'));
    assert.equal(s.length, 4);
    assert.equal(s.options[3].value, 'dv');
    s.remove(3);
    assert.equal(s.length, 3);
    assert.equal(s.type, 'select-one');
    s.multiple = true;
    assert.equal(s.type, 'select-multiple');
    const o = new Option('text', 'val', true, true);
    assert.ok(o.defaultSelected && o.selected);
    assert.equal(o.text, 'text');
});

test('textarea', () => {
    const t = document.createElement('textarea');
    t.textContent = 'def';
    assert.equal(t.value, 'def');
    t.value = 'x';
    assert.equal(t.value, 'x');
    assert.equal(t.defaultValue, 'def');
    assert.equal(t.textLength, 1);
    t.setSelectionRange(0, 1);
    assert.equal(t.selectionEnd, 1);
});

test('form elements, FormData, reset and validity', () => {
    const f = document.createElement('form');
    f.innerHTML = '<input name=a value=1><input name=b type=checkbox checked value=on><input name=c type=checkbox>' +
        '<select name=s><option>x</option><option selected>y</option></select><textarea name=t>tt</textarea>' +
        '<input name=d disabled value=no><fieldset disabled><input name=e value=no2></fieldset><input type=submit name=go value=Go>' +
        '<input name=req required>';
    document.body.appendChild(f);
    assert.equal(f.elements.length, 10, 'listed elements include the fieldset');
    assert.equal(f.elements.a.value, '1');
    const fd = new FormData(f);
    assert.deepEqual([...fd.keys()], ['a', 'b', 's', 't', 'req']);
    assert.equal(fd.get('s'), 'y');
    f.elements.a.value = 'changed';
    f.reset();
    assert.equal(f.elements.a.value, '1');
    assert.ok(!f.checkValidity());
    assert.ok(f.elements.req.validity.valueMissing);
    f.elements.req.value = 'x';
    assert.ok(f.checkValidity());
    const em = document.createElement('input');
    em.type = 'email';
    em.value = 'nope';
    assert.ok(em.validity.typeMismatch);
    assert.equal(f.elements.a.form, f);
    f.remove();
});

test('tables', () => {
    const t = document.createElement('table');
    const r = t.insertRow();
    r.insertCell().textContent = 'a';
    r.insertCell().textContent = 'b';
    assert.equal(t.rows.length, 1);
    assert.equal(t.tBodies.length, 1);
    assert.equal(r.cells.length, 2);
    assert.equal(r.cells[1].cellIndex, 1);
    assert.equal(r.rowIndex, 0);
    t.createTHead().insertRow().insertCell();
    assert.equal(t.rows.length, 2);
    assert.equal(t.rows[0].parentNode.localName, 'thead');
    t.createCaption().textContent = 'cap';
    assert.equal(t.firstChild.localName, 'caption');
    t.deleteRow(0);
    assert.equal(t.rows.length, 1);
    assert.throws(() => t.deleteRow(10), 'IndexSizeError');
});

test('innerText', () => {
    const d = document.createElement('div');
    document.body.appendChild(d);
    d.innerHTML = '<p>one  two</p><p>three<br>four</p><script>x</script><span hidden>no</span>';
    assert.equal(d.innerText, 'one two\n\nthree\nfour');
    d.innerText = 'a\nb';
    assert.equal(d.innerHTML, 'a<br>b');
    d.remove();
});

test('details toggle and dialog', () => {
    const det = document.createElement('details');
    det.innerHTML = '<summary>s</summary>body';
    document.body.appendChild(det);
    det.querySelector('summary').click();
    assert.ok(det.open);
    det.remove();
    const dlg = document.createElement('dialog');
    document.body.appendChild(dlg);
    dlg.showModal();
    assert.ok(dlg.open);
    assert.ok(dlg.matches(':modal'));
    dlg.showModal(); /* already modal: nothing happens */
    let closed = false;
    dlg.addEventListener('close', () => { closed = true; });
    dlg.close('rv');
    assert.ok(!dlg.open && closed);
    assert.ok(!dlg.matches(':modal'));
    assert.equal(dlg.returnValue, 'rv');
    dlg.show();
    assert.ok(dlg.open && !dlg.matches(':modal'));
    assert.throws(() => dlg.showModal()); /* open, but not modal */
    dlg.close();
    dlg.showModal();
    dlg.removeAttribute('open');
    assert.ok(!dlg.matches(':modal'));
    dlg.remove();
});

test('Image and template content', () => {
    const img = new Image(10, 20);
    assert.equal(img.localName, 'img');
    assert.equal(img.getAttribute('width'), '10');
    const t = document.createElement('template');
    t.innerHTML = '<b>x</b>';
    assert.equal(t.content.firstChild.localName, 'b');
    assert.equal(t.childNodes.length, 0);
});

test('script text and supports', () => {
    const s = document.createElement('script');
    s.text = 'var x;';
    assert.equal(s.textContent, 'var x;');
    assert.ok(HTMLScriptElement.supports('module'));
    assert.ok(!HTMLScriptElement.supports('nonsense'));
});
