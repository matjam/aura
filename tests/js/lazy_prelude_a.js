// The modules the prelude reads on first use, first touched here through
// typeof (Range), a member of another interface (document.styleSheets)
// and instanceof (CanvasRenderingContext2D). lazy_prelude_b.js and
// lazy_prelude_c.js touch them the other ways, each in a fresh window.
'use strict';

test('nothing is read before script touches it, and every global is visible', () => {
    for (const mod of ['range', 'cssom', 'canvas']) {
        assert.ok(lazyPending(mod), mod + ' is not read at start');
        lazyVisible(mod);
        assert.ok(lazyPending(mod), mod + ' is still not read after feature detection');
    }
});

test('range: typeof reads it', () => {
    assert.equal(typeof Range, 'function');
    lazyLoaded('range');
    assert.ok(lazyPending('cssom') && lazyPending('canvas'), 'the other modules stay unread');
    const r = document.createRange();
    assert.ok(r instanceof Range && r instanceof AbstractRange, 'createRange gives a Range');
    assert.equal(Object.prototype.toString.call(r), '[object Range]');
    assert.equal(Range.START_TO_END, 1);
    assert.ok(getSelection() instanceof Selection, 'getSelection');
    assert.ok(window.getSelection() === document.getSelection(), 'one Selection');
    assert.throws(() => new AbstractRange(), 'TypeError');
});

test('cssom: document.styleSheets reads it', () => {
    const s = document.createElement('style');
    s.textContent = 'p { color: red }';
    document.head.appendChild(s);
    const list = document.styleSheets;
    lazyLoaded('cssom');
    assert.ok(list instanceof StyleSheetList, 'a StyleSheetList');
    assert.ok(list.length >= 1 && list[list.length - 1] instanceof CSSStyleSheet, 'the style element\'s sheet');
    assert.ok(s.sheet === list[list.length - 1], 'element.sheet is the same object');
    assert.ok(s.sheet.cssRules[0] instanceof CSSStyleRule, 'its rule');
    assert.equal(s.sheet.cssRules[0].selectorText, 'p');
    assert.ok(lazyPending('canvas'), 'the canvas module stays unread');
});

test('canvas: instanceof reads it', () => {
    assert.ok(!({} instanceof CanvasRenderingContext2D), 'a plain object is not a context');
    lazyLoaded('canvas');
    const c = document.createElement('canvas');
    const ctx = c.getContext('2d');
    assert.ok(ctx instanceof CanvasRenderingContext2D, 'getContext gives a context');
    assert.equal(Object.prototype.toString.call(ctx), '[object CanvasRenderingContext2D]');
    assert.ok(ctx.getImageData(0, 0, 1, 1) instanceof ImageData, 'ImageData');
    assert.ok(ctx.getTransform() instanceof DOMMatrix, 'DOMMatrix');
    assert.ok(new Path2D() instanceof Path2D, 'Path2D');
    assert.throws(() => new CanvasRenderingContext2D(), 'TypeError');
    assert.equal(HTMLCanvasElement.prototype.getContext.name, 'getContext');
    assert.equal(HTMLCanvasElement.prototype.getContext.length, 2);
});
