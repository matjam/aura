// The parser looks ahead from a bracket to its match to tell an arrow
// function's parameters from a parenthesized expression and a
// destructuring target from a literal. QuickJS patch 0011 records what
// that look-ahead found for every bracket it passed, and returns the
// record when the parser reaches the bracket. These cases nest brackets
// of every kind, longer than the 32 bytes from which results are
// recorded, so that inner brackets are answered from the record.

test('arrow functions nested in parenthesized expressions', () => {
    const f = ((a, b) => ((c, d) => ((e) => [a, b, c, d, e, (0, ((x) => x * 2))(e)])(5))(3, 4))(1, 2);
    assert.deepEqual(f, [1, 2, 3, 4, 5, 10]);
    const g = (((((((((((((1 + 2) * 3) - 4) + 5) * 6) - 7) + 8) * 9) - 10) + 11) * 12) - 13));
    assert.equal(g, 6587);
    const h = (async (p = (q) => q + 1, [r, s] = [(t => t)(2), 3]) => p(r) + s);
    assert.ok(h() instanceof Promise);
});

test('destructuring targets among object and array literals', () => {
    let a, b, c, d, rest;
    ({ a, b: { c: [d, ...rest] } } = { a: 1, b: { c: [2, 3, 4, (() => 5)()] } });
    assert.deepEqual([a, d, rest], [1, 2, [3, 4, 5]]);
    const lit = { x: [{ y: ({ z: [1, 2, 3] }).z.map((v) => ({ v, w: [v, v * 2] })) }] };
    assert.equal(lit.x[0].y[2].w[1], 6);
    [a, [b, { c }]] = [10, [20, { c: (((30))) }]];
    assert.deepEqual([a, b, c], [10, 20, 30]);
    const o = { m(p = { q: [1, 2, 3].map((n) => n * 10) }) { return p.q; } };
    assert.deepEqual(o.m(), [10, 20, 30]);
});

test('templates, regular expressions and statements inside brackets', () => {
    const t = ((v) => `a${(v + 1) * 2}b${[v, (w) => w].length}c${`${(((v)))}`}`)(4);
    assert.equal(t, 'a10b2c4');
    const r = ((s) => s.replace(/[(){}[\]]+/g, '').split(/,/).map((x) => (x.length)))('(a,{bb},[ccc])');
    assert.deepEqual(r, [1, 2, 3]);
    const fns = [function () { for (let i = 0; i < 2; i++) { if ((i)) return (i, (() => i * 7)()); } }];
    assert.equal(fns[0](), 7);
    const big = { k: (() => { const x = { a: (1), b: [2, (3)] }; return (x.a + x.b[0] + x.b[1]); })() };
    assert.equal(big.k, 6);
});

test('a bracket followed by of, an arrow on the next line, and in', () => {
    const out = [];
    for (const [k, v] of [[1, 2], [3, (4)]].map(([x, y]) => [x * 10, y * 10])) out.push(k + v);
    assert.deepEqual(out, [30, 70]);
    let threw = false;
    try { new Function('return (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa, bbbbbbbbbbbbbb)\n=> 1'); } catch (e) { threw = e instanceof SyntaxError; }
    assert.ok(threw, 'a line break before => is a syntax error');
    const obj = { ['k' + (1 + 1)]: 'v' };
    assert.ok(('k2' in obj) && (((('k2')) in obj)));
});
