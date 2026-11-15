// CSSOM: style sheets and rules. This runner has no document view, so the
// sheets here are those of style elements, read from their text; that
// rule changes reach the cascade is checked in pages/sheets.html.
'use strict';

function styleEl(css) {
    const s = document.createElement('style');
    s.textContent = css;
    document.head.appendChild(s);
    return s;
}

test('a style element has a sheet while it is in the document', () => {
    const s = document.createElement('style');
    s.textContent = 'p { color: red }';
    assert.equal(s.sheet, null);
    document.head.appendChild(s);
    const sh = s.sheet;
    assert.ok(sh instanceof CSSStyleSheet && sh instanceof StyleSheet);
    assert.ok(sh === s.sheet, 'the same object each time');
    assert.ok(sh.ownerNode === s);
    assert.equal(sh.type, 'text/css');
    assert.equal(sh.href, null);
    assert.equal(sh.parentStyleSheet, null);
    assert.equal(sh.disabled, false);
    const list = document.styleSheets;
    assert.ok(list instanceof StyleSheetList);
    assert.ok(Array.from(list).includes(sh));
    assert.ok(list.item(list.length - 1) === sh && list[list.length - 1] === sh);
    s.remove();
    assert.equal(s.sheet, null);
    assert.ok(!Array.from(document.styleSheets).includes(sh));
    const other = document.createElement('style');
    other.type = 'text/x-other';
    document.head.appendChild(other);
    assert.equal(other.sheet, null, 'not CSS');
    other.remove();
    assert.throws(() => new CSSStyleSheet(), TypeError);
});

test('cssRules lists the rules by kind', () => {
    const s = styleEl(`/* a comment with { braces } */
        @import url("a.css") screen;
        @namespace svg url(http://www.w3.org/2000/svg);
        p, li > a[title="x{y}"] { color: red; margin: 0 }
        @media (min-width: 100px) { .a { top: 1px } .b { top: 2px } }
        @font-face { font-family: "F"; src: url(f.woff) }
        @keyframes spin { from { opacity: 0 } 50%, 60% { opacity: .5 } to { opacity: 1 } }
        @supports (display: grid) { .g { display: grid } }
        .x::after { content: "} not the end"; }
        @layer base { .l { left: 0 } }
        broken { `);
    const rules = s.sheet.cssRules;
    assert.ok(rules instanceof CSSRuleList);
    assert.equal(Array.from(rules).map((r) => r.constructor.name).join(), 'CSSImportRule,CSSNamespaceRule,CSSStyleRule,CSSMediaRule,' +
        'CSSFontFaceRule,CSSKeyframesRule,CSSSupportsRule,CSSStyleRule,CSSLayerBlockRule,CSSStyleRule');
    assert.equal(Array.from(rules).map((r) => r.type).join(), '3,10,1,4,5,7,12,1,0,1');
    assert.ok(s.sheet.rules.length === rules.length);
    const [imp, ns, st, media, face, kf, sup, after] = rules;
    assert.equal(imp.href, 'a.css');
    assert.equal(imp.media.mediaText, 'screen');
    assert.equal(imp.styleSheet, null);
    assert.equal(ns.prefix, 'svg');
    assert.equal(ns.namespaceURI, 'http://www.w3.org/2000/svg');
    assert.equal(st.selectorText, 'p, li > a[title="x{y}"]');
    assert.equal(st.style.color, 'rgb(255, 0, 0)');
    assert.equal(st.style.getPropertyValue('margin-top'), '0px');
    assert.ok(st.style.parentRule === st);
    assert.ok(st.parentStyleSheet === s.sheet && st.parentRule === null);
    assert.equal(st.type, CSSRule.STYLE_RULE);
    assert.equal(media.conditionText, '(min-width: 100px)');
    assert.equal(media.media.mediaText, '(min-width: 100px)');
    assert.equal(media.cssRules.length, 2);
    assert.equal(media.cssRules[1].selectorText, '.b');
    assert.ok(media.cssRules[0].parentRule === media);
    assert.equal(face.style.getPropertyValue('font-family'), '"F"');
    assert.equal(kf.name, 'spin');
    assert.equal(kf.cssRules.length, 3);
    assert.equal(kf.cssRules[1].keyText, '50%, 60%');
    assert.equal(kf.cssRules[1].style.opacity, '0.5');
    assert.ok(kf.findRule('100%') === kf.cssRules[2]);
    assert.equal(sup.conditionText, '(display: grid)');
    assert.equal(after.selectorText, '.x::after');
    assert.equal(after.style.content, '"} not the end"');
    assert.ok(/^p, li > a\[title="x\{y\}"\] \{/.test(st.cssText));
    assert.equal(rules.item(99), null);
    s.remove();
});

test('insertRule and deleteRule', () => {
    const s = styleEl('a { color: red } b { color: blue }');
    const sh = s.sheet;
    assert.equal(sh.insertRule('i { top: 1px }'), 0);
    assert.equal(sh.insertRule('u { top: 2px }', 3), 3);
    assert.equal(Array.from(sh.cssRules).map((r) => r.selectorText).join(), 'i,a,b,u');
    assert.throws(() => sh.insertRule('x { }', 9), 'IndexSizeError');
    assert.throws(() => sh.insertRule('not a rule'), 'SyntaxError');
    assert.throws(() => sh.insertRule('a:nonsense(1) { }'), 'SyntaxError');
    assert.throws(() => sh.insertRule('a { } b { }'), 'SyntaxError');
    assert.throws(() => sh.insertRule(''), 'SyntaxError');
    assert.throws(() => sh.insertRule('@import "x.css";', 2), 'HierarchyRequestError');
    assert.equal(sh.insertRule('@media print { p { top: 0 } }', 4), 4);
    assert.equal(sh.cssRules[4].cssRules[0].selectorText, 'p');
    sh.deleteRule(0);
    assert.equal(sh.cssRules[0].selectorText, 'a');
    assert.throws(() => sh.deleteRule(9), 'IndexSizeError');
    assert.equal(sh.addRule('q', 'left: 0'), -1);
    assert.equal(sh.cssRules[sh.cssRules.length - 1].selectorText, 'q');
    sh.removeRule(0);
    assert.equal(sh.cssRules[0].selectorText, 'b');
    assert.equal(s.textContent, 'a { color: red } b { color: blue }', "the element's text is as it was");
    /* a feature test many libraries run */
    let ok = true;
    try { sh.insertRule(':focus-visible { outline: 0 }'); } catch (e) { ok = false; }
    assert.ok(ok);
    /* new text in the element makes a new rule list */
    s.textContent = 'z { top: 0 }';
    assert.equal(sh.cssRules.length, 1);
    assert.equal(sh.cssRules[0].selectorText, 'z');
    s.remove();
});

test('changing a rule', () => {
    const s = styleEl('a { color: red } @media screen { b { top: 1px } } @keyframes k { from { left: 0 } to { left: 9px } }');
    const sh = s.sheet;
    const a = sh.cssRules[0];
    a.style.color = 'blue';
    a.style.setProperty('margin-left', '3px', 'important');
    assert.equal(a.style.color, 'rgb(0, 0, 255)');
    assert.ok(/color: rgb\(0, 0, 255\)/.test(a.cssText) && /margin-left: 3px !important/.test(a.cssText), a.cssText);
    assert.ok(sh.cssRules[0] === a, 'the rule object stays');
    a.selectorText = 'a.big, a.small';
    assert.equal(a.selectorText, 'a.big, a.small');
    a.selectorText = 'a:nonsense';
    assert.equal(a.selectorText, 'a.big, a.small', 'an invalid selector is ignored');
    a.style.removeProperty('color');
    assert.ok(!/color/.test(a.cssText));
    const media = sh.cssRules[1];
    media.insertRule('i { top: 2px }', 1);
    media.cssRules[0].style.top = '5px';
    assert.equal(media.cssRules.length, 2);
    assert.ok(/@media screen \{\s*b \{ top: 5px; \}\s*i \{ top: 2px \}\s*\}/.test(media.cssText), media.cssText);
    media.deleteRule(0);
    assert.equal(media.cssRules[0].selectorText, 'i');
    media.media.mediaText = 'print';
    assert.equal(media.conditionText, 'print');
    const k = sh.cssRules[2];
    k.appendRule('50% { left: 4px }');
    assert.equal(k.cssRules.length, 3);
    k.deleteRule('from');
    assert.equal(k.cssRules[0].keyText, 'to');
    assert.ok(/@keyframes k \{/.test(k.cssText));
    s.remove();
});

test('style takes every shorthand the engine has, white-space among them', () => {
    const d = document.createElement('div');
    for (const v of ['nowrap', 'pre', 'pre-wrap', 'pre-line', 'normal', 'break-spaces']) {
        d.style.whiteSpace = v;
        assert.equal(d.style.whiteSpace, v);
        assert.ok(/white-space-collapse/.test(d.getAttribute('style')), 'the style attribute holds it');
    }
    d.style.fontVariant = 'small-caps';
    assert.ok(/font-variant-caps: small-caps/.test(d.getAttribute('style')));
    assert.ok('whiteSpace' in d.style && 'marginBlock' in d.style && !('bogusProperty' in d.style));
});

test('sheet.disabled and media', () => {
    const s = styleEl('a { color: red }');
    s.media = 'print';
    const sh = s.sheet;
    assert.equal(sh.media.mediaText, 'print');
    assert.equal(sh.media.length, 1);
    assert.equal(sh.media.item(0), 'print');
    sh.media.appendMedium('screen');
    assert.equal(s.getAttribute('media'), 'print, screen');
    sh.media.deleteMedium('print');
    assert.equal(sh.media.mediaText, 'screen');
    assert.throws(() => sh.media.deleteMedium('tv'), 'NotFoundError');
    sh.disabled = true;
    assert.equal(sh.disabled, true);
    sh.disabled = false;
    s.remove();
});
