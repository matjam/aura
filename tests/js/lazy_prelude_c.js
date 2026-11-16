// The modules the prelude reads on first use, first touched here through
// instanceof (Range), typeof (CSSRule) and a member of another interface
// (a canvas element's getContext); and a page that replaces a global
// before its module is read keeps its own value. See lazy_prelude_a.js.
'use strict';

test('a global the page replaces before the module is read stays replaced', () => {
    assert.ok(lazyPending('range'));
    window.StaticRange = 42;
    assert.equal(StaticRange, 42, 'the page\'s value');
    assert.ok(!(document.body instanceof Range), 'instanceof reads the module');
    assert.equal(StaticRange, 42, 'still the page\'s value after the module is read');
    const d = Object.getOwnPropertyDescriptor(window, 'Range');
    assert.ok('value' in d && typeof d.value === 'function', 'the other globals become data properties');
    assert.ok(document.createRange() instanceof Range);
});

test('cssom: typeof reads it', () => {
    assert.ok(lazyPending('cssom'));
    assert.equal(typeof CSSRule, 'function');
    lazyLoaded('cssom');
    assert.equal(CSSRule.STYLE_RULE, 1);
    const l = document.createElement('link');
    assert.equal(l.sheet, null, 'a link that is not a style sheet has none');
});

test('canvas: getContext reads it', () => {
    assert.ok(lazyPending('canvas'));
    const c = document.createElement('canvas');
    c.width = 8; /* the width and height setters work before the module is read */
    assert.equal(c.width, 8);
    assert.ok(lazyPending('canvas'), 'setting the size does not read it');
    const ctx = c.getContext('2d');
    lazyLoaded('canvas');
    assert.ok(ctx instanceof CanvasRenderingContext2D, 'a context');
    assert.ok(c.getContext('2d') === ctx, 'the same context each time');
    assert.ok(c.toDataURL().startsWith('data:image/png'), 'toDataURL');
    c.width = 8; /* resets the bitmap through the context */
    assert.equal(c.width, 8);
});
