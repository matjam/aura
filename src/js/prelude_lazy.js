/* ---------------- Range, CSSOM and the canvas, read on first use ----------------
 * The three are modules of their own (range.js, cssom.js, canvas.js; see
 * lazyModule in prelude.js). Here: their globals as accessors, and the
 * members of other interfaces that reach them, each of which loads its
 * module when called. Their names, lengths and property flags are those
 * the modules' own definitions had. */

const ranges = lazyModule(0, ['AbstractRange', 'Range', 'StaticRange', 'Selection', 'getSelection'],
    () => ({ N, doc, DOMRect, Event, S, domErr, fire, isNode, rootOf, str, tag, selectionMoved: editCaretMoved }));

const cssom = lazyModule(1, ['CSSRule', 'CSSStyleRule', 'CSSGroupingRule', 'CSSConditionRule', 'CSSMediaRule',
    'CSSSupportsRule', 'CSSContainerRule', 'CSSLayerBlockRule', 'CSSImportRule', 'CSSFontFaceRule', 'CSSPageRule',
    'CSSNamespaceRule', 'CSSKeyframeRule', 'CSSKeyframesRule', 'CSSRuleList', 'MediaList', 'StyleSheet', 'CSSStyleSheet',
    'StyleSheetList', 'CSSNestedDeclarations'], () => ({ N, doc, S, domErr, str, tag }));
defProps(HTMLStyleElement.prototype, { get sheet() { return cssom().sheetFor(this); } });
defProps(HTMLLinkElement.prototype, { get sheet() { return cssom().sheetFor(this); } });
const adoptedProps = {
    get adoptedStyleSheets() { return cssom().adoptedGet(this); },
    set adoptedStyleSheets(v) { cssom().adoptedSet(this, v); },
};
defProps(DP, { get styleSheets() { return cssom().styleSheetsOf(this, true); } });
defProps(DP, adoptedProps);
if (typeof ShadowRoot === 'function') {
    defProps(ShadowRoot.prototype, adoptedProps);
    defProps(ShadowRoot.prototype, { get styleSheets() { return cssom().styleSheetsOf(this, false); } });
}

const canvases = lazyModule(2, ['CanvasRenderingContext2D', 'CanvasGradient', 'CanvasPattern', 'ImageBitmap',
    'TextMetrics', 'Path2D', 'ImageData', 'DOMMatrixReadOnly', 'DOMMatrix', 'OffscreenCanvas', 'createImageBitmap'],
    () => ({ N, Blob, defProps, domErr, str, tag }));
defProps(HTMLCanvasElement.prototype, {
    getContext(type, opts) { return canvases().getContext(this, type, opts); },
    toDataURL() { return canvases().toDataURL(this); },
    toBlob(cb) { return canvases().toBlob(this, cb); },
});
/* Setting width or height resets the bitmap, also when the value is the
 * one it has. The attribute is written only when it changes, so that a
 * script clearing its canvas with canvas.width = canvas.width costs no
 * layout. (A canvas has a context, _c2d, only once canvas.js is loaded.) */
for (const name of ['width', 'height']) {
    const d = Object.getOwnPropertyDescriptor(HTMLCanvasElement.prototype, name);
    Object.defineProperty(HTMLCanvasElement.prototype, name, {
        get: d.get,
        set(v) {
            if (this.hasAttribute(name) && d.get.call(this) === Number(v)) {
                const c = this._c2d;
                if (c) N.canvasResize(c, this.width, this.height);
                return;
            }
            d.set.call(this, v);
            N.canvasSync();
        },
        configurable: true,
        enumerable: true,
    });
}
