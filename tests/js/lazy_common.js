// Shared by the lazy_prelude_*.js tests: the globals of the three modules
// the prelude reads on first use (src/js/range.js, cssom.js, canvas.js,
// lazyModule in prelude.js), and checks that read a global's descriptor
// without calling its getter.
'use strict';

const LAZY = {
    range: ['AbstractRange', 'Range', 'StaticRange', 'Selection', 'getSelection'],
    cssom: ['CSSRule', 'CSSStyleRule', 'CSSGroupingRule', 'CSSConditionRule', 'CSSMediaRule', 'CSSSupportsRule',
        'CSSContainerRule', 'CSSLayerBlockRule', 'CSSImportRule', 'CSSFontFaceRule', 'CSSPageRule', 'CSSNamespaceRule',
        'CSSKeyframeRule', 'CSSKeyframesRule', 'CSSRuleList', 'MediaList', 'StyleSheet', 'CSSStyleSheet', 'StyleSheetList'],
    canvas: ['CanvasRenderingContext2D', 'CanvasGradient', 'CanvasPattern', 'ImageBitmap', 'TextMetrics', 'Path2D',
        'ImageData', 'DOMMatrixReadOnly', 'DOMMatrix', 'OffscreenCanvas', 'createImageBitmap'],
};

/* true while the module has not been read: every global is still the
 * accessor (reading the descriptor does not call the getter) */
function lazyPending(mod) {
    return LAZY[mod].every((name) => {
        const d = Object.getOwnPropertyDescriptor(globalThis, name);
        return d && typeof d.get === 'function' && d.get === d.set && !d.enumerable && d.configurable;
    });
}

/* After loading: each global a plain, non-enumerable, writable function. */
function lazyLoaded(mod) {
    for (const name of LAZY[mod]) {
        const d = Object.getOwnPropertyDescriptor(globalThis, name);
        assert.ok(d && 'value' in d, name + ' is a data property after the module is read');
        assert.equal(typeof d.value, 'function', name);
        assert.equal(d.enumerable, false, name + ' is not enumerable');
        assert.equal(d.writable, true, name + ' is writable');
    }
}

/* Feature detection sees every global before the module is read. */
function lazyVisible(mod) {
    const own = Object.getOwnPropertyNames(globalThis);
    for (const name of LAZY[mod]) {
        assert.ok(name in globalThis, name + ' in window');
        assert.ok(globalThis.hasOwnProperty(name), name + ' is an own property of window');
        assert.ok(own.includes(name), name + ' among the window\'s own property names');
    }
    const seen = [];
    for (const k in globalThis) seen.push(k);
    for (const name of LAZY[mod]) assert.ok(!seen.includes(name), name + ' is not enumerated, as before');
}
