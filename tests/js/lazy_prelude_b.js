// The modules the prelude reads on first use, first touched here through
// a member of another interface (document.createRange), instanceof
// (CSSStyleSheet) and typeof (DOMMatrix). See lazy_prelude_a.js.
'use strict';

test('range: document.createRange reads it', () => {
    assert.ok(lazyPending('range'));
    const r = document.createRange();
    lazyLoaded('range');
    assert.ok(r instanceof Range, 'a Range');
    r.selectNodeContents(document.body);
    assert.ok(r.startContainer === document.body, 'it works');
    assert.equal(typeof StaticRange, 'function');
});

test('cssom: instanceof reads it, and a constructed sheet can be adopted', () => {
    assert.ok(lazyPending('cssom'));
    assert.ok(!(document instanceof CSSStyleSheet), 'a document is not a sheet');
    lazyLoaded('cssom');
    const sheet = new CSSStyleSheet();
    sheet.replaceSync('div { margin: 1px }');
    document.adoptedStyleSheets = [sheet];
    assert.equal(document.adoptedStyleSheets.length, 1, 'adopted');
    assert.ok(document.adoptedStyleSheets[0] === sheet, 'the same sheet');
    assert.ok(sheet.cssRules[0] instanceof CSSRule, 'its rule is a CSSRule');
    document.adoptedStyleSheets = [];
});

test('canvas: typeof reads it', () => {
    assert.ok(lazyPending('canvas'));
    assert.equal(typeof DOMMatrix, 'function');
    lazyLoaded('canvas');
    const m = new DOMMatrix([1, 0, 0, 1, 5, 6]);
    assert.ok(m instanceof DOMMatrixReadOnly, 'DOMMatrix extends DOMMatrixReadOnly');
    assert.equal(m.e, 5);
    assert.ok(new OffscreenCanvas(4, 4).getContext('2d') instanceof CanvasRenderingContext2D, 'OffscreenCanvas');
});
