// QuickJS patch 0016: a pattern that is a run of one-character matchers
// followed by $ (no m, y, u or v flag) is tried only at the one place a
// match can start, the end of the input less its length. These cases
// check that the answers are the same as trying every place.

test('end-anchored patterns match as before', () => {
    const cases = [
        [/[\n ]$/, 'abc ', true], [/[\n ]$/, 'abc', false], [/[\n ]$/, '', false], [/[\n ]$/, ' ', true],
        [/ab$/, 'xxab', true], [/ab$/, 'abx', false], [/a.c$/, 'zzabc', true], [/a.c$/i, 'ZZABC', true],
        [/$/, 'abc', true], [/\s$/, 'a\t', true], [/\S$/, 'a ', false], [/[^a]$/, 'ab', true], [/[^a]$/, 'ba', false],
        [/abc$/, 'ab', false], [/.$/, '\n', false], [/[\s\S]$/, '\n', true],
    ];
    for (const [re, s, want] of cases) assert.equal(re.test(s), want, re + ' on ' + JSON.stringify(s));
});

test('flags that leave the search as it was', () => {
    assert.ok(/ab$/m.test('ab\ncd'), 'multiline: $ before a line break');
    assert.ok(!/c$/y.test('abc'), 'sticky: from lastIndex only');
    assert.ok(/😀$/u.test('x😀'), 'unicode: two code units');
    assert.ok(/(a)$/.test('ba'), 'a group is not shortened');
    assert.ok(/a+$/.test('baa'), 'a quantifier is not shortened');
});

test('positions and lastIndex', () => {
    assert.equal(/bc$/.exec('abcabc').index, 4);
    assert.equal('foo bar '.match(/[\n ]$/).index, 7);
    assert.equal('a b '.replace(/ $/, '!'), 'a b!');
    const g = /x$/g;
    g.lastIndex = 5;
    assert.ok(g.test('aaaaaaax'));
    assert.equal(g.lastIndex, 8);
    g.lastIndex = 9;
    assert.ok(!g.test('aaaaaaax'), 'lastIndex past the end');
    assert.deepEqual('a1b1'.split(/1$/), ['a1b', '']);
});

test('long input is not scanned', () => {
    const big = 'a'.repeat(100000) + ' ';
    const t = Date.now();
    for (let i = 0; i < 2000; i++) /[\n ]$/.test(big);
    assert.ok(Date.now() - t < 1000, 'took ' + (Date.now() - t) + ' ms');
});
