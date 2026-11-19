// QuickJS patch 0024: a regular expression may have more than 255
// capture groups (Discord's main script has one with 343). Capture
// indexes are 16-bit in the bytecode.

function groups(n, body) {
    let s = '';
    for (let i = 0; i < n; i++) s += '(' + body + ')';
    return s;
}

test('300 captures', () => {
    const re = new RegExp('^' + groups(300, 'a') + '(b)$');
    const m = re.exec('a'.repeat(300) + 'b');
    assert.equal(m.length, 302);
    assert.equal(m[1], 'a');
    assert.equal(m[300], 'a');
    assert.equal(m[301], 'b');
});

test('a back reference past 255', () => {
    const re = new RegExp(groups(400, 'a') + '(b)\\401');
    assert.equal(re.test('a'.repeat(400) + 'bb'), true);
    assert.equal(re.test('a'.repeat(400) + 'ba'), false);
});

test('named groups and a reset past 255', () => {
    const re = new RegExp(groups(260, 'x?') + '(?:(?<n>c)|d)+\\k<n>');
    const m = re.exec('dccdc');
    assert.equal(m.groups.n, undefined);
    const m2 = re.exec('cc');
    assert.equal(m2.groups.n, 'c');
});

test('a nested prefix tree like Discord\'s', () => {
    let s = '';
    for (let i = 0; i < 120; i++) s += (i ? '|' : '') + i + '(0(1|2)|3)';
    const re = new RegExp('^(' + s + ')$');
    assert.equal(re.test('1190'), false);
    assert.equal(re.test('11902'), true);
    assert.equal(re.test('73'), true);
    assert.equal(re.test('74'), false);
});

test('small patterns still work', () => {
    assert.equal(/(a)*b/.exec('aab')[1], 'a');
    assert.equal(/(?:(a)|b)+/.exec('ab')[1], undefined);
    assert.equal(/(\w)\1$/.test('xyy'), true);
    assert.equal(/(?<=(\d+)(\d+))$/.exec('1053')[1], '1');
    assert.equal(/x$/.test('ax'), true);
});

test('65535 captures is the limit', () => {
    let threw = false;
    try { new RegExp(groups(65535, '')); } catch (e) { threw = e instanceof SyntaxError; }
    assert.equal(threw, true);
});
