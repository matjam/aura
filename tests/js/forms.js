// Form controls: legacy named access, values and selection through the
// control state, RadioNodeList, validity, reset.

function setup(html) {
    document.body.innerHTML = html;
}

test('document.forms and document.<name>', () => {
    setup('<form name="frmLogin" id="f1"><input name="user" value="u"><input name="pass" type="password"></form>' +
          '<form name="second"></form><img name="logo"><embed name="movie">');
    const f = document.getElementById('f1');
    assert.equal(document.forms.length, 2);
    assert.equal(document.forms[0], f);
    assert.equal(document.forms.frmLogin, f);
    assert.equal(document.forms['second'], document.forms[1]);
    assert.equal(document.frmLogin, f);
    assert.equal(document.logo.localName, 'img');
    assert.equal(document.movie.localName, 'embed');
    assert.equal(document.nothingHere, undefined);
    // real properties win
    assert.equal(typeof document.createElement, 'function');
    // instanceof is unchanged by the named lookup
    assert.ok(document instanceof Document && document instanceof Node);
    assert.ok(f instanceof HTMLFormElement && f instanceof HTMLElement);
});

test('form.<control>, form.elements, form[index]', () => {
    setup('<form name="frmLogin"><input name="user" value="u"><input name="pass" type="password">' +
          '<input type="radio" name="r" value="a"><input type="radio" name="r" value="b" checked></form>');
    const f = document.forms.frmLogin;
    assert.equal(document.frmLogin.user.value, 'u');
    assert.equal(document.forms.frmLogin.user.focus !== undefined, true);
    assert.equal(f.pass.type, 'password');
    assert.equal(f.elements.user, f.user);
    assert.equal(f.elements['pass'], f.pass);
    assert.equal(f[0], f.user);
    assert.equal(f[1], f.pass);
    assert.equal(f.length, 4);
    assert.equal(f[9], undefined);
    const r = f.r;
    assert.ok(r instanceof RadioNodeList, 'RadioNodeList');
    assert.equal(r.length, 2);
    assert.equal(r.value, 'b');
    r.value = 'a';
    assert.equal(r[0].checked, true);
    assert.equal(r[1].checked, false);
    assert.ok(f.elements.namedItem('r') instanceof RadioNodeList);
    assert.equal(typeof f.submit, 'function');
});

test('legacy focus pattern runs', () => {
    setup('<form name="frmLogin"><input name="user"></form>');
    document.forms.frmLogin.user.focus();
    assert.equal(document.activeElement, document.frmLogin.user);
});

test('value, defaultValue and dirty flag', () => {
    setup('<input id="i" value="one"><textarea id="t">hello</textarea>');
    const i = document.getElementById('i'), t = document.getElementById('t');
    assert.equal(i.value, 'one');
    i.setAttribute('value', 'two');
    assert.equal(i.value, 'two', 'clean value follows the attribute');
    i.value = 'three';
    i.setAttribute('value', 'four');
    assert.equal(i.value, 'three', 'dirty value stays');
    assert.equal(i.defaultValue, 'four');
    assert.equal(t.value, 'hello');
    t.value = 'a\r\nb';
    assert.equal(t.value, 'a\nb');
    t.textContent = 'x';
    assert.equal(t.defaultValue, 'x');
    assert.equal(t.value, 'a\nb');
});

test('selection API in UTF-16 units', () => {
    setup('<input id="i" value="héllo 😀 w">');
    const i = document.getElementById('i');
    i.setSelectionRange(1, 3);
    assert.equal(i.selectionStart, 1);
    assert.equal(i.selectionEnd, 3);
    i.setSelectionRange(6, 8, 'backward');
    assert.equal(i.selectionStart, 6);
    assert.equal(i.selectionEnd, 8);
    assert.equal(i.selectionDirection, 'backward');
    i.select();
    assert.equal(i.selectionStart, 0);
    assert.equal(i.selectionEnd, i.value.length);
    i.setRangeText('X', 0, 1);
    assert.equal(i.value.slice(0, 2), 'Xé');
    const c = document.createElement('input');
    c.type = 'checkbox';
    assert.equal(c.selectionStart, null);
});

test('sanitization by type', () => {
    setup('<input id="n" type="number"><input id="r" type="range" min="0" max="10">');
    const n = document.getElementById('n'), r = document.getElementById('r');
    n.value = 'abc';
    assert.equal(n.value, '');
    n.value = '12.5';
    assert.equal(n.value, '12.5');
    assert.equal(r.value, '5');
    r.value = '99';
    assert.equal(r.value, '10');
    const e = document.createElement('input');
    e.value = 'a\nb\rc';
    assert.equal(e.value, 'abc');
});

test('validity and setCustomValidity', () => {
    setup('<form id="f"><input id="a" required><input id="b" pattern="[0-9]+" value="x1">' +
          '<input id="c" type="email" value="nope"><input id="d" maxlength="2"></form>');
    const a = document.getElementById('a'), b = document.getElementById('b'), c = document.getElementById('c');
    assert.equal(a.validity.valueMissing, true);
    assert.equal(a.checkValidity(), false);
    assert.equal(b.validity.patternMismatch, true);
    assert.equal(c.validity.typeMismatch, true);
    let invalid = 0;
    a.addEventListener('invalid', () => invalid++);
    assert.equal(document.getElementById('f').checkValidity(), false);
    assert.equal(invalid, 1);
    a.value = 'x';
    b.value = '42';
    c.value = 'a@b.c';
    assert.equal(document.getElementById('f').checkValidity(), true);
    a.setCustomValidity('nope');
    assert.equal(a.validity.customError, true);
    assert.equal(a.validationMessage, 'nope');
    a.setCustomValidity('');
    assert.equal(a.validity.valid, true);
});

test('reset restores defaults', () => {
    setup('<form id="f"><input id="i" value="d"><input id="k" type="checkbox" checked>' +
          '<select id="s"><option>a</option><option selected>b</option></select></form>');
    const f = document.getElementById('f');
    const i = document.getElementById('i'), k = document.getElementById('k'), s = document.getElementById('s');
    i.value = 'changed';
    k.checked = false;
    s.selectedIndex = 0;
    let resets = 0;
    f.addEventListener('reset', () => resets++);
    f.reset();
    assert.equal(resets, 1);
    assert.equal(i.value, 'd');
    assert.equal(k.checked, true);
    assert.equal(s.selectedIndex, 1);
});

test('checkbox click and preventDefault', () => {
    setup('<input id="k" type="checkbox">');
    const k = document.getElementById('k');
    const seen = [];
    k.addEventListener('click', () => seen.push('click:' + k.checked));
    k.addEventListener('input', () => seen.push('input'));
    k.addEventListener('change', () => seen.push('change'));
    k.click();
    assert.deepEqual(seen, ['click:true', 'input', 'change']);
    k.addEventListener('click', (e) => e.preventDefault());
    k.click();
    assert.equal(k.checked, true, 'cancelled click restores the state');
});

test('submit event and submitter', () => {
    setup('<form id="f"><input name="q" value="x"><button id="b" name="go" value="1">Go</button></form>');
    const f = document.getElementById('f');
    let sub = null;
    f.addEventListener('submit', (e) => { sub = e.submitter; e.preventDefault(); });
    document.getElementById('b').click();
    assert.equal(sub, document.getElementById('b'));
    assert.throws(() => f.requestSubmit(document.createElement('button')), 'TypeError');
});
