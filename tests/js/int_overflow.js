// QuickJS patch 0023: an int32 add, subtract or multiply that overflows
// makes its double from the two operands in double arithmetic, not from
// the 64-bit result. The values must be the same.

test('add and subtract past int32', () => {
    const max = 2147483647, min = -2147483648;
    assert.equal(max + 1, 2147483648);
    assert.equal(min - 1, -2147483649);
    assert.equal(min + min, -4294967296);
    assert.equal(max - min, 4294967295);
    let x = max;
    x += 1; // add_loc
    assert.equal(x, 2147483648);
});

test('products past int32 round as a double', () => {
    const max = 2147483647, min = -2147483648;
    assert.equal(max * max, 4611686014132420609);
    assert.equal(min * min, 4611686018427387904);
    assert.equal(min * -1, 2147483648);
    assert.equal(65536 * 65536, 4294967296);
    assert.equal(46341 * 46341, 2147488281);
    assert.equal(String(1234567891 * 1987654321), '2453894203114007000');
    assert.equal(String(1234567891 * -1987654321), '-2453894203114007000');
    assert.equal(String(min * max), '-4611686016279904000');
    assert.equal(1 / (-1 * 0), -Infinity, 'still -0');
});

test('a hash that wraps', () => {
    let h = 0;
    for (let i = 0; i < 1000; i++) h = (h * 31 + i) | 0;
    assert.equal(h, 562641396);
});
