
/* ---------------- HTML elements: reflection ---------------- */

const HEP = HTMLElement.prototype;
const resolveUrl = (v) => { const r = N.urlParse(str(v), null); return r ? r[0] : str(v); };

/* Reflected attributes, as compact tables: [property, kind, attribute,
 * extra]. Kinds: s string, b boolean, u URL, l long (extra: default),
 * n non-negative long (extra: default), e enumerated (extra:
 * [keywords, missing default, invalid default]), d double (extra:
 * default), t token list (the DOMTokenList property; extra: supported
 * tokens). */
function reflect(proto, rows) {
    const props = {};
    for (const [prop, kind, attrName, extra] of rows) {
        const attr = attrName || prop.toLowerCase();
        let d;
        switch (kind) {
        case 's':
            d = { get() { return this.getAttribute(attr) ?? ''; }, set(v) { this.setAttribute(attr, str(v)); } };
            break;
        case 'b':
            d = { get() { return this.hasAttribute(attr); }, set(v) { this.toggleAttribute(attr, !!v); } };
            break;
        case 'u':
            d = {
                get() { const v = this.getAttribute(attr); return v === null ? '' : resolveUrl(v); },
                set(v) { this.setAttribute(attr, str(v)); },
            };
            break;
        case 'l': case 'n': {
            const def = extra === undefined ? (kind === 'n' ? 0 : 0) : extra;
            d = {
                get() {
                    const v = this.getAttribute(attr);
                    if (v === null) return def;
                    const m = /^[\t\n\f\r ]*([+-]?[0-9]+)/.exec(v);
                    if (!m) return def;
                    const n = parseInt(m[1], 10);
                    if (n < -2147483648 || n > 2147483647 || (kind === 'n' && n < 0)) return def;
                    return n;
                },
                set(v) {
                    v = Number(v) | 0;
                    if (kind === 'n' && v < 0) throw domErr('The value must not be negative', 'IndexSizeError');
                    this.setAttribute(attr, String(v));
                },
            };
            break;
        }
        case 'd': {
            const def = extra || 0;
            d = {
                get() { const v = parseFloat(this.getAttribute(attr)); return isFinite(v) ? v : def; },
                set(v) { v = Number(v); if (!isFinite(v)) throw new TypeError('The value is not finite'); this.setAttribute(attr, String(v)); },
            };
            break;
        }
        case 'e': {
            const [kw, missing, invalid] = extra;
            d = {
                get() {
                    const v = this.getAttribute(attr);
                    if (v === null) return missing;
                    const l = v.toLowerCase();
                    return kw.includes(l) ? l : invalid;
                },
                set(v) { this.setAttribute(attr, str(v)); },
            };
            break;
        }
        case 't':
            d = {
                get() { return tokenList(this, attr, extra); },
                set(v) { this.setAttribute(attr, str(v)); },
            };
            break;
        }
        props[prop] = d;
    }
    for (const k of Object.keys(props)) Object.defineProperty(proto, k, Object.assign(props[k], { configurable: true, enumerable: true }));
}

reflect(HEP, [['title', 's'], ['lang', 's'], ['translate', 'e', 0, [['yes', 'no'], 'yes', 'yes']],
    ['dir', 'e', 0, [['ltr', 'rtl', 'auto'], '', '']], ['hidden', 'b'], ['inert', 'b'], ['accessKey', 's', 'accesskey'],
    ['autofocus', 'b'], ['nonce', 's'], ['autocapitalize', 's'], ['enterKeyHint', 's', 'enterkeyhint'],
    ['inputMode', 's', 'inputmode']]);
/* SVG and MathML elements have style, dataset and tabIndex too
 * (ElementCSSInlineStyle, HTMLOrSVGElement); React sets svg.style. */
for (const proto of [SVGElement.prototype, MathMLElement.prototype]) {
    defProps(proto, {
        get dataset() { return datasetOf(this); },
        get style() { return styleOf(this); },
        set style(v) { this.setAttribute('style', str(v)); },
        get tabIndex() {
            const v = this.getAttribute('tabindex');
            return v !== null && /^\s*[+-]?\d+/.test(v) ? parseInt(v, 10) : this.localName === 'a' ? 0 : -1;
        },
        set tabIndex(v) { this.setAttribute('tabindex', String(Number(v) | 0)); },
    });
}
defProps(HEP, {
    get dataset() { return datasetOf(this); },
    get style() { return styleOf(this); },
    set style(v) { this.setAttribute('style', str(v)); },
    get tabIndex() {
        const v = this.getAttribute('tabindex');
        if (v !== null && /^\s*[+-]?\d+/.test(v)) return parseInt(v, 10);
        const n = this.localName;
        if (/^(a|area|button|frame|iframe|input|object|select|textarea)$/.test(n)) return 0;
        if (n === 'summary' && this.parentElement && this.parentElement.localName === 'details' &&
            childOfType(this.parentElement, 'summary') === this) return 0;
        return -1;
    },
    set tabIndex(v) { this.setAttribute('tabindex', String(Number(v) | 0)); },
    get draggable() { const v = (this.getAttribute('draggable') || '').toLowerCase(); return v === 'true' || (v !== 'false' && (this.localName === 'img' || (this.localName === 'a' && this.hasAttribute('href')))); },
    set draggable(v) { this.setAttribute('draggable', v ? 'true' : 'false'); },
    get spellcheck() { return (this.getAttribute('spellcheck') || '').toLowerCase() !== 'false'; },
    set spellcheck(v) { this.setAttribute('spellcheck', v ? 'true' : 'false'); },
    get contentEditable() {
        const v = this.getAttribute('contenteditable');
        if (v === null) return 'inherit';
        const l = v.toLowerCase();
        return l === '' || l === 'true' ? 'true' : l === 'false' ? 'false' : l === 'plaintext-only' ? 'plaintext-only' : 'inherit';
    },
    set contentEditable(v) {
        const l = str(v).toLowerCase();
        if (l === 'inherit') this.removeAttribute('contenteditable');
        else if (l === 'true' || l === 'false' || l === 'plaintext-only') this.setAttribute('contenteditable', l);
        else throw domErr('Invalid contentEditable value', 'SyntaxError');
    },
    get isContentEditable() {
        for (let e = this; e; e = e.parentElement) {
            const v = e.getAttribute('contenteditable');
            if (v !== null) return v.toLowerCase() !== 'false';
        }
        return false;
    },
    get innerText() { return innerTextOf(this); },
    set innerText(v) {
        v = str(v);
        const d = this.ownerDocument;
        const f = d.createDocumentFragment();
        const lines = v.split(/\r\n|\r|\n/);
        lines.forEach((l, i) => { if (i) f.appendChild(d.createElement('br')); if (l) f.appendChild(d.createTextNode(l)); });
        this.replaceChildren(f);
    },
    get outerText() { return this.innerText; },
    set outerText(v) { const t = this.ownerDocument.createTextNode(str(v)); this.replaceWith(t); },
    click() {
        if (this.hasAttribute('disabled') && isFormControl(this)) return;
        if (this[S.clicking]) return;
        this[S.clicking] = true;
        try {
            activate(this, new MouseEvent('click', { bubbles: true, cancelable: true, composed: true, view: G }));
        } finally {
            this[S.clicking] = false;
        }
    },
    attachInternals() { throw domErr('ElementInternals is not supported', 'NotSupportedError'); },
});
S.clicking = Symbol('clicking');

/* Element methods pages call without checking (docs/web-platform.md,
 * item 5). checkVisibility: false without a box, and with the options,
 * for a hidden visibility or an ancestor of opacity 0. Fullscreen is
 * refused. Pointer capture is recorded but does not yet redirect events
 * (known-gaps.md). */
const pointerCaptures = new Map(); /* pointerId -> element */
defProps(Element.prototype, {
    checkVisibility(opts) {
        if (!this.isConnected || !N.rect(this)) return false;
        opts = opts || {};
        if (opts.checkVisibilityCSS || opts.visibilityProperty) {
            if (G.getComputedStyle(this).visibility !== 'visible') return false;
        }
        if (opts.checkOpacity || opts.opacityProperty) {
            for (let e = this; e; e = e.parentElement)
                if (G.getComputedStyle(e).opacity === '0') return false;
        }
        return true;
    },
    requestFullscreen() { return Promise.reject(new TypeError('Fullscreen is not supported')); },
    setPointerCapture(id) {
        if (!this.isConnected) throw domErr('The element is not in a document', 'InvalidStateError');
        pointerCaptures.set(Number(id), this);
    },
    releasePointerCapture(id) {
        if (pointerCaptures.get(Number(id)) === this) pointerCaptures.delete(Number(id));
    },
    hasPointerCapture(id) { return pointerCaptures.get(Number(id)) === this; },
});

/* innerText without layout: block-level elements break lines, br is a
 * newline, display:none (hidden attribute, script, style, head) is
 * skipped, and white space collapses outside pre. */
const blockTags = new Set(['address', 'article', 'aside', 'blockquote', 'details', 'dialog', 'dd', 'div', 'dl', 'dt',
    'fieldset', 'figcaption', 'figure', 'footer', 'form', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hgroup', 'hr',
    'li', 'main', 'nav', 'ol', 'p', 'pre', 'section', 'table', 'ul', 'tr', 'summary', 'center', 'body', 'html', 'caption']);
const hiddenTags = new Set(['script', 'style', 'head', 'template', 'noscript', 'title', 'meta', 'link']);
function innerTextOf(root) {
    if (!root.isConnected || hiddenTags.has(root.localName)) return root.textContent;
    let out = '';
    let pendingBreak = 0;
    const emit = (s) => {
        if (!s) return;
        if (pendingBreak && out) out += '\n'.repeat(pendingBreak);
        pendingBreak = 0;
        out += s;
    };
    const stack = [[root, false]];
    while (stack.length) {
        const [n, closing] = stack.pop();
        if (closing) {
            if (n.localName === 'p') pendingBreak = Math.max(pendingBreak, 2);
            else if (blockTags.has(n.localName)) pendingBreak = Math.max(pendingBreak, 1);
            else if (n.localName === 'td' || n.localName === 'th') { if (n.nextElementSibling) emit('\t'); }
            continue;
        }
        if (n.nodeType === 3) {
            let pre = false;
            for (let p = n.parentElement; p && p !== root.parentElement; p = p.parentElement)
                if (p.localName === 'pre' || p.localName === 'textarea') { pre = true; break; }
            let t = n.data;
            if (!pre) {
                t = t.replace(/[\t\n\r ]+/g, ' ');
                if (!out || /[\n ]$/.test(out) || pendingBreak) t = t.replace(/^ /, '');
            }
            emit(t);
            continue;
        }
        if (n.nodeType !== 1 && n !== root) continue;
        if (n !== root) {
            if (hiddenTags.has(n.localName) || n.hasAttribute('hidden')) continue;
            if (n.localName === 'br') { out = out.replace(/ $/, ''); out += pendingBreak ? '\n'.repeat(pendingBreak) : ''; pendingBreak = 0; out += '\n'; continue; }
            if (n.localName === 'p') pendingBreak = Math.max(pendingBreak, out ? 2 : 0);
            else if (blockTags.has(n.localName) && out) pendingBreak = Math.max(pendingBreak, 1);
        }
        stack.push([n, true]);
        for (let c = n.lastChild; c; c = c.previousSibling) stack.push([c, false]);
    }
    return out.replace(/ +\n/g, '\n').replace(/ $/, '');
}

const ifaceRows = {
    HTMLAnchorElement: [['target', 's'], ['download', 's'], ['ping', 's'], ['rel', 's'], ['relList', 't', 'rel'],
        ['hreflang', 's'], ['type', 's'], ['referrerPolicy', 's', 'referrerpolicy'], ['coords', 's'], ['charset', 's'],
        ['name', 's'], ['rev', 's'], ['shape', 's']],
    HTMLAreaElement: [['alt', 's'], ['coords', 's'], ['shape', 's'], ['target', 's'], ['download', 's'], ['ping', 's'],
        ['rel', 's'], ['relList', 't', 'rel'], ['referrerPolicy', 's', 'referrerpolicy'], ['noHref', 'b', 'nohref']],
    HTMLMediaElement: [['src', 'u'], ['crossOrigin', 'e', 'crossorigin', [['anonymous', 'use-credentials'], null, 'anonymous']],
        ['preload', 's'], ['autoplay', 'b'], ['loop', 'b'], ['controls', 'b'], ['defaultMuted', 'b', 'muted']],
    HTMLVideoElement: [['width', 'n'], ['height', 'n'], ['poster', 'u'], ['playsInline', 'b', 'playsinline']],
    HTMLBRElement: [['clear', 's']],
    HTMLBaseElement: [['target', 's']],
    HTMLBodyElement: [['text', 's'], ['link', 's'], ['vLink', 's', 'vlink'], ['aLink', 's', 'alink'], ['bgColor', 's', 'bgcolor'],
        ['background', 's']],
    HTMLButtonElement: [['disabled', 'b'], ['formAction', 'u', 'formaction'], ['formEnctype', 's', 'formenctype'],
        ['formMethod', 's', 'formmethod'], ['formNoValidate', 'b', 'formnovalidate'], ['formTarget', 's', 'formtarget'],
        ['name', 's'], ['value', 's'], ['type', 'e', 0, [['submit', 'reset', 'button'], 'submit', 'submit']],
        ['popoverTargetAction', 'e', 'popovertargetaction', [['toggle', 'show', 'hide'], 'toggle', 'toggle']]],
    HTMLCanvasElement: [['width', 'n', 0, 300], ['height', 'n', 0, 150]],
    HTMLDListElement: [['compact', 'b']],
    HTMLDataElement: [['value', 's']],
    HTMLDetailsElement: [['open', 'b'], ['name', 's']],
    HTMLDialogElement: [['open', 'b']],
    HTMLDivElement: [['align', 's']],
    HTMLEmbedElement: [['src', 'u'], ['type', 's'], ['width', 's'], ['height', 's'], ['align', 's'], ['name', 's']],
    HTMLFieldSetElement: [['disabled', 'b'], ['name', 's']],
    HTMLFormElement: [['acceptCharset', 's', 'accept-charset'], ['autocomplete', 'e', 0, [['on', 'off'], 'on', 'on']],
        ['enctype', 'e', 0, [['application/x-www-form-urlencoded', 'multipart/form-data', 'text/plain'],
            'application/x-www-form-urlencoded', 'application/x-www-form-urlencoded']],
        ['method', 'e', 0, [['get', 'post', 'dialog'], 'get', 'get']], ['name', 's'], ['noValidate', 'b', 'novalidate'],
        ['target', 's'], ['rel', 's'], ['relList', 't', 'rel']],
    HTMLFrameElement: [['src', 'u'], ['name', 's'], ['scrolling', 's'], ['frameBorder', 's', 'frameborder'],
        ['noResize', 'b', 'noresize'], ['longDesc', 'u', 'longdesc'], ['marginHeight', 's', 'marginheight'],
        ['marginWidth', 's', 'marginwidth']],
    HTMLFrameSetElement: [['cols', 's'], ['rows', 's']],
    HTMLHeadingElement: [['align', 's']],
    HTMLHRElement: [['align', 's'], ['color', 's'], ['noShade', 'b', 'noshade'], ['size', 's'], ['width', 's']],
    HTMLHtmlElement: [['version', 's']],
    HTMLIFrameElement: [['src', 'u'], ['srcdoc', 's'], ['name', 's'], ['sandbox', 't', 'sandbox', []], ['allow', 's'],
        ['allowFullscreen', 'b', 'allowfullscreen'], ['width', 's'], ['height', 's'], ['referrerPolicy', 's', 'referrerpolicy'],
        ['loading', 'e', 0, [['lazy', 'eager'], 'eager', 'eager']], ['align', 's'], ['scrolling', 's'],
        ['frameBorder', 's', 'frameborder'], ['longDesc', 'u', 'longdesc'], ['marginHeight', 's', 'marginheight'],
        ['marginWidth', 's', 'marginwidth']],
    HTMLImageElement: [['alt', 's'], ['src', 'u'], ['srcset', 's'], ['sizes', 's'],
        ['crossOrigin', 'e', 'crossorigin', [['anonymous', 'use-credentials'], null, 'anonymous']], ['useMap', 's', 'usemap'],
        ['isMap', 'b', 'ismap'], ['referrerPolicy', 's', 'referrerpolicy'], ['decoding', 'e', 0, [['sync', 'async', 'auto'], 'auto', 'auto']],
        ['loading', 'e', 0, [['lazy', 'eager'], 'eager', 'eager']], ['fetchPriority', 'e', 'fetchpriority', [['high', 'low', 'auto'], 'auto', 'auto']],
        ['name', 's'], ['lowsrc', 'u'], ['align', 's'], ['hspace', 'n'], ['vspace', 'n'], ['longDesc', 'u', 'longdesc'],
        ['border', 's']],
    HTMLInputElement: [['accept', 's'], ['alt', 's'], ['autocomplete', 's'], ['defaultChecked', 'b', 'checked'],
        ['dirName', 's', 'dirname'], ['disabled', 'b'], ['formAction', 'u', 'formaction'], ['formEnctype', 's', 'formenctype'],
        ['formMethod', 's', 'formmethod'], ['formNoValidate', 'b', 'formnovalidate'], ['formTarget', 's', 'formtarget'],
        ['height', 'n'], ['max', 's'], ['maxLength', 'n', 'maxlength', -1], ['min', 's'], ['minLength', 'n', 'minlength', -1],
        ['multiple', 'b'], ['name', 's'], ['pattern', 's'], ['placeholder', 's'], ['readOnly', 'b', 'readonly'],
        ['required', 'b'], ['size', 'n', 0, 20], ['src', 'u'], ['step', 's'], ['defaultValue', 's', 'value'], ['width', 'n'],
        ['align', 's'], ['useMap', 's', 'usemap'],
        ['popoverTargetAction', 'e', 'popovertargetaction', [['toggle', 'show', 'hide'], 'toggle', 'toggle']]],
    HTMLLIElement: [['value', 'l'], ['type', 's']],
    HTMLLabelElement: [['htmlFor', 's', 'for']],
    HTMLLegendElement: [['align', 's']],
    HTMLLinkElement: [['href', 'u'], ['crossOrigin', 'e', 'crossorigin', [['anonymous', 'use-credentials'], null, 'anonymous']],
        ['rel', 's'], ['relList', 't', 'rel', ['stylesheet', 'preload', 'modulepreload', 'icon', 'alternate', 'prefetch', 'dns-prefetch', 'preconnect', 'next', 'search', 'canonical', 'manifest']],
        ['as', 's'], ['media', 's'], ['integrity', 's'], ['hreflang', 's'], ['type', 's'], ['sizes', 't'],
        ['referrerPolicy', 's', 'referrerpolicy'], ['disabled', 'b'], ['charset', 's'], ['rev', 's'], ['target', 's'],
        ['imageSrcset', 's', 'imagesrcset'], ['imageSizes', 's', 'imagesizes'], ['fetchPriority', 's', 'fetchpriority']],
    HTMLMapElement: [['name', 's']],
    HTMLMenuElement: [['compact', 'b']],
    HTMLMetaElement: [['name', 's'], ['httpEquiv', 's', 'http-equiv'], ['content', 's'], ['media', 's'], ['scheme', 's']],
    HTMLMeterElement: [['value', 'd'], ['min', 'd'], ['max', 'd', 0, 1], ['low', 'd'], ['high', 'd'], ['optimum', 'd']],
    HTMLModElement: [['cite', 'u'], ['dateTime', 's', 'datetime']],
    HTMLOListElement: [['reversed', 'b'], ['start', 'l', 0, 1], ['type', 's'], ['compact', 'b']],
    HTMLObjectElement: [['data', 'u'], ['type', 's'], ['name', 's'], ['useMap', 's', 'usemap'], ['width', 's'], ['height', 's'],
        ['align', 's'], ['archive', 's'], ['code', 's'], ['declare', 'b'], ['hspace', 'n'], ['standby', 's'], ['vspace', 'n'],
        ['codeBase', 'u', 'codebase'], ['codeType', 's', 'codetype'], ['border', 's']],
    HTMLOptGroupElement: [['disabled', 'b'], ['label', 's']],
    HTMLOptionElement: [['disabled', 'b'], ['defaultSelected', 'b', 'selected']],
    HTMLOutputElement: [['name', 's'], ['htmlFor', 't', 'for']],
    HTMLParagraphElement: [['align', 's']],
    HTMLPreElement: [['width', 'l']],
    HTMLProgressElement: [['max', 'd', 0, 1]],
    HTMLQuoteElement: [['cite', 'u']],
    HTMLScriptElement: [['src', 'u'], ['type', 's'], ['noModule', 'b', 'nomodule'], ['async', 'b'], ['defer', 'b'],
        ['crossOrigin', 'e', 'crossorigin', [['anonymous', 'use-credentials'], null, 'anonymous']], ['integrity', 's'],
        ['referrerPolicy', 's', 'referrerpolicy'], ['charset', 's'], ['event', 's'], ['htmlFor', 's', 'for'],
        ['fetchPriority', 's', 'fetchpriority']],
    HTMLSelectElement: [['autocomplete', 's'], ['disabled', 'b'], ['multiple', 'b'], ['name', 's'], ['required', 'b'], ['size', 'n']],
    HTMLSlotElement: [['name', 's']],
    HTMLSourceElement: [['src', 'u'], ['type', 's'], ['srcset', 's'], ['sizes', 's'], ['media', 's'], ['width', 'n'], ['height', 'n']],
    HTMLStyleElement: [['media', 's'], ['type', 's'], ['disabled', 'b'], ['blocking', 't']],
    HTMLTableElement: [['align', 's'], ['border', 's'], ['frame', 's'], ['rules', 's'], ['summary', 's'], ['width', 's'],
        ['bgColor', 's', 'bgcolor'], ['cellPadding', 's', 'cellpadding'], ['cellSpacing', 's', 'cellspacing']],
    HTMLTableCaptionElement: [['align', 's']],
    HTMLTableCellElement: [['colSpan', 'n', 'colspan', 1], ['rowSpan', 'n', 'rowspan', 1], ['headers', 's'], ['abbr', 's'],
        ['scope', 'e', 0, [['row', 'col', 'rowgroup', 'colgroup'], '', '']], ['align', 's'], ['axis', 's'], ['height', 's'],
        ['width', 's'], ['ch', 's', 'char'], ['chOff', 's', 'charoff'], ['noWrap', 'b', 'nowrap'], ['vAlign', 's', 'valign'],
        ['bgColor', 's', 'bgcolor']],
    HTMLTableColElement: [['span', 'n', 0, 1], ['align', 's'], ['ch', 's', 'char'], ['chOff', 's', 'charoff'],
        ['vAlign', 's', 'valign'], ['width', 's']],
    HTMLTableSectionElement: [['align', 's'], ['ch', 's', 'char'], ['chOff', 's', 'charoff'], ['vAlign', 's', 'valign']],
    HTMLTableRowElement: [['align', 's'], ['ch', 's', 'char'], ['chOff', 's', 'charoff'], ['vAlign', 's', 'valign'],
        ['bgColor', 's', 'bgcolor']],
    HTMLTextAreaElement: [['autocomplete', 's'], ['cols', 'n', 0, 20], ['dirName', 's', 'dirname'], ['disabled', 'b'],
        ['maxLength', 'n', 'maxlength', -1], ['minLength', 'n', 'minlength', -1], ['name', 's'], ['placeholder', 's'],
        ['readOnly', 'b', 'readonly'], ['required', 'b'], ['rows', 'n', 0, 2], ['wrap', 's']],
    HTMLTimeElement: [['dateTime', 's', 'datetime']],
    HTMLTrackElement: [['kind', 's'], ['src', 'u'], ['srclang', 's'], ['label', 's'], ['default', 'b']],
    HTMLUListElement: [['compact', 'b'], ['type', 's']],
};
for (const name of Object.keys(ifaceRows)) reflect(G[name].prototype, ifaceRows[name]);

/* Hyperlink mixin (a, area): href and its URL parts. */
const hyperlink = {
    get href() { const v = this.getAttribute('href'); if (v === null) return ''; const r = N.urlParse(v, null); return r ? r[0] : v; },
    set href(v) { this.setAttribute('href', str(v)); },
    toString() { return this.href; },
};
const urlPartNames = ['protocol', 'username', 'password', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin'];
for (const k of urlPartNames) {
    Object.defineProperty(hyperlink, k, {
        get() {
            const v = this.getAttribute('href');
            if (v === null) return '';
            try { return new URL(v, N.docUrl(1))[k]; } catch (e) { return k === 'origin' ? '' : ''; }
        },
        set(x) {
            if (k === 'origin') return;
            const v = this.getAttribute('href');
            let u;
            try { u = new URL(v === null ? '' : v, N.docUrl(1)); } catch (e) { return; }
            u[k] = x;
            this.setAttribute('href', u.href);
        },
        enumerable: true, configurable: true,
    });
}
for (const C of [HTMLAnchorElement, HTMLAreaElement]) {
    for (const k of Reflect.ownKeys(hyperlink)) Object.defineProperty(C.prototype, k, Object.getOwnPropertyDescriptor(hyperlink, k));
}
defProps(HTMLAnchorElement.prototype, {
    get text() { return this.textContent; },
    set text(v) { this.textContent = v; },
});

/* ---------------- forms ---------------- */

const formControlTags = new Set(['button', 'fieldset', 'input', 'object', 'output', 'select', 'textarea']);
const isFormControl = (e) => isElement(e) && e.namespaceURI === HTML_NS && formControlTags.has(e.localName);
const ctrl = new WeakMap(); /* element -> {value, dirty, selected, ...} */
const ctrlState = (e) => { let s = ctrl.get(e); if (!s) ctrl.set(e, (s = {})); return s; };

function formOwner(e) {
    const id = e.getAttribute('form');
    if (id !== null) {
        const f = e.isConnected ? doc.getElementById(id) : null;
        return f && f.localName === 'form' ? f : null;
    }
    for (let p = e.parentElement; p; p = p.parentElement) if (p.localName === 'form' && p.namespaceURI === HTML_NS) return p;
    return null;
}
function isDisabled(e) {
    if (e.hasAttribute('disabled')) return true;
    for (let p = e.parentElement; p; p = p.parentElement) {
        if (p.localName === 'fieldset' && p.hasAttribute('disabled')) {
            const legend = childOfType(p, 'legend');
            if (!legend || !legend.contains(e)) return true;
        }
    }
    return false;
}

const inputTypes = ['hidden', 'text', 'search', 'tel', 'url', 'email', 'password', 'date', 'month', 'week', 'time',
    'datetime-local', 'number', 'range', 'color', 'checkbox', 'radio', 'file', 'submit', 'image', 'reset', 'button'];
const inputType = (e) => { const t = (e.getAttribute('type') || '').toLowerCase(); return inputTypes.includes(t) ? t : 'text'; };
const valueModeDefault = new Set(['hidden', 'submit', 'image', 'reset', 'button']);
const valueModeOn = new Set(['checkbox', 'radio']);
const sanitizeValue = (type, v) => {
    if (/^(text|search|tel|password)$/.test(type)) return v.replace(/[\r\n]/g, '');
    if (type === 'url' || type === 'email') return v.replace(/[\r\n]/g, '').trim();
    if (type === 'number') return /^-?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(v) ? v : '';
    if (type === 'range') {
        const n = parseFloat(v);
        return isFinite(n) ? String(n) : '50';
    }
    if (type === 'color') return /^#[0-9a-fA-F]{6}$/.test(v) ? v.toLowerCase() : '#000000';
    return v;
};

function isChecked(e) {
    const st = N.state(e);
    if (st & 64) return !!(st & 32); /* AU_STATE_CHECKED_SET */
    return e.hasAttribute('checked') || (e.localName === 'option' && e.hasAttribute('selected'));
}
function radioGroup(e) {
    const name = e.getAttribute('name');
    if (!name) return [e];
    const form = formOwner(e);
    const root = e.getRootNode();
    const all = root.querySelectorAll ? root.querySelectorAll('input') : [];
    return Array.from(all).filter((x) => inputType(x) === 'radio' && x.getAttribute('name') === name && formOwner(x) === form);
}
function setChecked(e, on) {
    N.setChecked(e, on);
    if (on && inputType(e) === 'radio') for (const r of radioGroup(e)) if (r !== e && isChecked(r)) N.setChecked(r, false);
}

defProps(HTMLInputElement.prototype, {
    get type() { return inputType(this); },
    set type(v) { this.setAttribute('type', str(v)); },
    /* The value and selection live in the control (src/dom/control.h),
     * shared with the document view's editing. */
    get value() {
        const t = inputType(this);
        if (valueModeDefault.has(t)) return this.getAttribute('value') ?? '';
        if (valueModeOn.has(t)) return this.getAttribute('value') ?? 'on';
        if (t === 'file') { const f = N.ctlFiles(this); return f.length ? 'C:\\fakepath\\' + f[0][0] : ''; }
        return N.ctlValue(this);
    },
    set value(v) {
        const t = inputType(this);
        v = v === null ? '' : str(v);
        if (valueModeDefault.has(t) || valueModeOn.has(t)) { this.setAttribute('value', v); return; }
        if (t === 'file') { if (v !== '') throw domErr('A file input can only be cleared', 'InvalidStateError'); N.ctlClearFiles(this); return; }
        N.ctlSetValue(this, v);
    },
    get valueAsNumber() {
        const t = inputType(this), v = this.value;
        if (t === 'number' || t === 'range') { const n = parseFloat(v); return isFinite(n) ? n : NaN; }
        if (t === 'date') { const d = Date.parse(v + 'T00:00:00Z'); return isFinite(d) ? d : NaN; }
        return NaN;
    },
    set valueAsNumber(n) {
        const t = inputType(this);
        if (t === 'number' || t === 'range') this.value = isFinite(n) ? String(n) : '';
        else if (t === 'date') this.value = isFinite(n) ? new Date(n).toISOString().slice(0, 10) : '';
        else throw domErr('valueAsNumber does not apply', 'InvalidStateError');
    },
    get valueAsDate() { const t = inputType(this); if (t !== 'date') return null; const d = Date.parse(this.value + 'T00:00:00Z'); return isFinite(d) ? new Date(d) : null; },
    set valueAsDate(d) { if (inputType(this) !== 'date') throw domErr('valueAsDate does not apply', 'InvalidStateError'); this.value = d ? d.toISOString().slice(0, 10) : ''; },
    get checked() { return isChecked(this); },
    set checked(v) { setChecked(this, !!v); },
    get indeterminate() { return !!(N.state(this) & 128); },
    set indeterminate(v) { ctrlState(this).indeterminate = !!v; },
    get files() {
        if (inputType(this) !== 'file') return null;
        const fs = N.ctlFiles(this);
        if (!fs.length) return cachedList(this, 'files', emptyFileList);
        const fl = Object.create(FileList.prototype);
        fs.forEach((f, i) => Object.defineProperty(fl, i, { value: new File([f[2]], f[0], { type: f[1] }), enumerable: true }));
        Object.defineProperty(fl, 'length', { value: fs.length, enumerable: true });
        return Object.freeze(fl);
    },
    get list() { const id = this.getAttribute('list'); const e = id && doc.getElementById(id); return e && e.localName === 'datalist' ? e : null; },
    stepUp(n = 1) { stepBy(this, n); },
    stepDown(n = 1) { stepBy(this, -n); },
    showPicker() {},
});
/* Text selection of text inputs and textareas (UTF-16 offsets; the
 * control keeps UTF-8 ones). Other input types have none. */
const hasTextSel = (e) => e.localName === 'textarea' || /^(text|search|tel|url|password)$/.test(inputType(e));
const textSelection = {
    select() { N.ctlSetSel(this, 0, 0x7FFFFFFF, false); fireSimple(this, 'select', true, false); },
    get selectionStart() { return hasTextSel(this) ? N.ctlSel(this)[0] : null; },
    set selectionStart(v) { const s = N.ctlSel(this); v >>>= 0; N.ctlSetSel(this, v, Math.max(v, s[1]), false); },
    get selectionEnd() { return hasTextSel(this) ? N.ctlSel(this)[1] : null; },
    set selectionEnd(v) { const s = N.ctlSel(this); v >>>= 0; N.ctlSetSel(this, Math.min(s[0], v), v, false); },
    get selectionDirection() { return hasTextSel(this) ? (N.ctlSel(this)[2] ? 'backward' : 'forward') : null; },
    set selectionDirection(d) { const s = N.ctlSel(this); N.ctlSetSel(this, s[0], s[1], d === 'backward'); },
    setSelectionRange(a, b, d) {
        if (!hasTextSel(this)) throw domErr('The input element does not support selection', 'InvalidStateError');
        N.ctlSetSel(this, a >>> 0, b >>> 0, d === 'backward');
    },
    setRangeText(text, start, end, mode) {
        const v = this.value, sel = N.ctlSel(this);
        let a = sel[0], b = sel[1];
        if (start !== undefined) { a = Math.min(start >>> 0, v.length); b = Math.min(end >>> 0, v.length); }
        if (a > b) throw domErr('start is after end', 'IndexSizeError');
        text = str(text);
        N.ctlSetValue(this, v.slice(0, a) + text + v.slice(b));
        const m = mode || 'preserve';
        if (m === 'select') N.ctlSetSel(this, a, a + text.length, false);
        else if (m === 'start') N.ctlSetSel(this, a, a, false);
        else if (m === 'end') N.ctlSetSel(this, a + text.length, a + text.length, false);
        else {
            const d = text.length - (b - a);
            const fix = (x) => (x > b ? x + d : x > a ? a : x);
            N.ctlSetSel(this, fix(sel[0]), start === undefined ? a + text.length : fix(sel[1]), false);
        }
    },
};
defProps(HTMLInputElement.prototype, textSelection);
function stepBy(e, n) {
    const t = inputType(e);
    if (t !== 'number' && t !== 'range') throw domErr('stepUp does not apply', 'InvalidStateError');
    const step = parseFloat(e.getAttribute('step')) || 1;
    let v = (parseFloat(e.value) || 0) + step * n;
    const min = parseFloat(e.getAttribute('min')), max = parseFloat(e.getAttribute('max'));
    if (isFinite(max) && v > max) v = max;
    if (isFinite(min) && v < min) v = min;
    e.value = String(v);
}

defProps(HTMLTextAreaElement.prototype, {
    get type() { return 'textarea'; },
    get defaultValue() { return this.textContent; },
    set defaultValue(v) { this.textContent = v; },
    get value() { return N.ctlValue(this); },
    set value(v) { N.ctlSetValue(this, v === null ? '' : str(v)); },
    get textLength() { return this.value.length; },
});
defProps(HTMLTextAreaElement.prototype, textSelection);

/* option and select */
/* Selectedness is the option's checked state, shared with the view. */
function optionSelected(o) { return isChecked(o); }
function selectOptions(sel) { return N.collect(sel, 6); }
function selectedIndexOf(sel) {
    const opts = selectOptions(sel);
    for (let i = 0; i < opts.length; i++) if (optionSelected(opts[i])) return i;
    if (!sel.multiple && (sel.size | 0) <= 1) for (let i = 0; i < opts.length; i++) if (!opts[i].disabled) return i;
    return -1;
}
function setOptionSelected(o, on) { N.setChecked(o, on); }
defProps(HTMLOptionElement.prototype, {
    get selected() {
        const sel = this.closest('select');
        if (sel && !sel.multiple) return selectOptions(sel)[selectedIndexOf(sel)] === this;
        return optionSelected(this);
    },
    set selected(v) {
        const sel = this.closest('select');
        if (v && sel && !sel.multiple) for (const o of selectOptions(sel)) if (o !== this) setOptionSelected(o, false);
        setOptionSelected(this, !!v);
    },
    get value() { return this.getAttribute('value') ?? this.text; },
    set value(v) { this.setAttribute('value', str(v)); },
    get text() { return this.textContent.replace(/[\t\n\f\r ]+/g, ' ').trim(); },
    set text(v) { this.textContent = v; },
    get label() { return this.getAttribute('label') ?? this.text; },
    set label(v) { this.setAttribute('label', str(v)); },
    get index() { const sel = this.closest('select'); return sel ? selectOptions(sel).indexOf(this) : 0; },
    get form() { const sel = this.closest('select'); return sel ? formOwner(sel) : null; },
});
defProps(HTMLSelectElement.prototype, {
    get type() { return this.multiple ? 'select-multiple' : 'select-one'; },
    get options() { return cachedList(this, 'options', () => makeList(HTMLOptionsCollection, this, 6)); },
    get length() { return selectOptions(this).length; },
    set length(n) { this.options.length = n; },
    get selectedOptions() { return makeList(HTMLCollection, this, -1, null, selectOptions(this).filter((o) => o.selected)); },
    get selectedIndex() { return selectedIndexOf(this); },
    set selectedIndex(i) {
        const opts = selectOptions(this);
        for (let k = 0; k < opts.length; k++) setOptionSelected(opts[k], k === (i | 0));
    },
    get value() { const o = selectOptions(this)[selectedIndexOf(this)]; return o ? o.value : ''; },
    set value(v) {
        v = str(v);
        let done = false;
        for (const o of selectOptions(this)) { const m = !done && o.value === v; setOptionSelected(o, m); if (m) done = true; }
    },
    item(i) { return selectOptions(this)[i >>> 0] || null; },
    namedItem(n) { return this.options.namedItem(n); },
    add(el, before) {
        if (typeof before === 'number') before = selectOptions(this)[before] || null;
        if (before) before.parentNode.insertBefore(el, before); else this.appendChild(el);
    },
    remove(i) {
        if (arguments.length === 0) { childMixin.remove.call(this); return; }
        const o = selectOptions(this)[i | 0];
        if (o) o.remove();
    },
    [Symbol.iterator]() { return selectOptions(this)[Symbol.iterator](); },
});

/* Shared form-associated members. */
const formAssoc = {
    get form() { return formOwner(this); },
    get labels() {
        if (this.localName === 'input' && inputType(this) === 'hidden') return null;
        const id = this.id;
        const out = [];
        for (const l of doc.getElementsByTagName('label')) if (l.control === this) out.push(l);
        return makeList(NodeList, this, -1, null, out);
    },
    get willValidate() { return !isDisabled(this) && !this.hasAttribute('readonly') && !(this.localName === 'input' && /^(hidden|reset|button)$/.test(inputType(this))) && this.localName !== 'fieldset' && this.localName !== 'output'; },
    get validity() { return validityOf(this); },
    get validationMessage() { return this.willValidate ? validationMessageOf(this) : ''; },
    checkValidity() { const ok = !this.willValidate || validityOf(this).valid; if (!ok) fireSimple(this, 'invalid', false, true); return ok; },
    reportValidity() {
        const ok = !this.willValidate || validityOf(this).valid;
        if (!ok && fire(this, new Event('invalid', { cancelable: true })) && this.isConnected) this.focus();
        return ok;
    },
    setCustomValidity(m) { N.ctlCustom(this, str(m)); },
};
for (const C of [HTMLInputElement, HTMLButtonElement, HTMLSelectElement, HTMLTextAreaElement, HTMLOutputElement,
    HTMLFieldSetElement, HTMLObjectElement]) defProps(C.prototype, formAssoc);
defProps(HTMLLabelElement.prototype, {
    get control() {
        const f = this.getAttribute('for');
        if (f !== null) {
            const e = this.isConnected ? doc.getElementById(f) : null;
            return e && labelable(e) ? e : null;
        }
        for (const e of this.querySelectorAll('*')) if (labelable(e)) return e;
        return null;
    },
    get form() { const c = this.control; return c ? formOwner(c) : null; },
});
const labelable = (e) => /^(button|input|meter|output|progress|select|textarea)$/.test(e.localName) &&
    !(e.localName === 'input' && inputType(e) === 'hidden');
defProps(HTMLFieldSetElement.prototype, {
    get type() { return 'fieldset'; },
    get elements() { return makeList(HTMLCollection, this, 5); },
});
defProps(HTMLOutputElement.prototype, {
    get type() { return 'output'; },
    get value() { return this.textContent; },
    set value(v) { this.textContent = v; },
    get defaultValue() { const s = ctrl.get(this); return s && 'def' in s ? s.def : this.textContent; },
    set defaultValue(v) { ctrlState(this).def = str(v); },
});
defProps(HTMLLegendElement.prototype, { get form() { const f = this.parentElement; return f && f.localName === 'fieldset' ? formOwner(f) : null; } });
defProps(HTMLProgressElement.prototype, {
    get value() { const v = parseFloat(this.getAttribute('value')); return isFinite(v) && v > 0 ? Math.min(v, this.max) : 0; },
    set value(v) { this.setAttribute('value', String(+v)); },
    get position() { return this.hasAttribute('value') ? this.value / this.max : -1; },
});

class ValidityState { constructor() { throw new TypeError('Illegal constructor'); } }
tag(ValidityState);
defGlobal('ValidityState', ValidityState);
function validityOf(e) {
    const v = Object.create(ValidityState.prototype);
    const val = 'value' in e ? e.value : '';
    const s = { custom: N.ctlCustom(e), dirty: N.ctlDirty(e) && N.ctlInteracted(e) };
    const t = e.localName === 'input' ? inputType(e) : '';
    const f = {
        valueMissing: e.hasAttribute('required') && (t === 'checkbox' ? !e.checked :
            t === 'radio' ? !radioGroup(e).some(isChecked) : val === ''),
        typeMismatch: val !== '' && ((t === 'email' && !/^[^@\s]+@[^@\s]+$/.test(val)) || (t === 'url' && !N.urlParse(val))),
        patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false,
        stepMismatch: false, badInput: false, customError: !!s.custom,
    };
    const pat = e.getAttribute('pattern');
    if (pat !== null && val !== '') { try { f.patternMismatch = !new RegExp('^(?:' + pat + ')$', 'v').test(val); } catch (x) { /* invalid pattern: ignored */ } }
    if (s.dirty) {
        const maxl = parseInt(e.getAttribute('maxlength'), 10), minl = parseInt(e.getAttribute('minlength'), 10);
        f.tooLong = maxl >= 0 && val.length > maxl;
        f.tooShort = minl > 0 && val.length > 0 && val.length < minl;
    }
    if ((t === 'number' || t === 'range') && val !== '') {
        const n = +val, min = parseFloat(e.getAttribute('min')), max = parseFloat(e.getAttribute('max'));
        f.rangeUnderflow = isFinite(min) && n < min;
        f.rangeOverflow = isFinite(max) && n > max;
    }
    if (t === 'file' && e.hasAttribute('required')) f.valueMissing = N.ctlFiles(e).length === 0;
    if (e.localName === 'select' && e.hasAttribute('required')) {
        const o = selectOptions(e), i = selectedIndexOf(e);
        f.valueMissing = i < 0 || (!e.multiple && (e.size | 0) <= 1 && i === 0 && o[0].value === '' && o[0].parentNode === e);
    }
    f.valid = !Object.keys(f).some((k) => f[k]);
    for (const k of Object.keys(f)) Object.defineProperty(v, k, { value: f[k], enumerable: true });
    return v;
}
function validationMessageOf(e) {
    const v = validityOf(e), t = e.localName === 'input' ? inputType(e) : e.localName;
    if (v.customError) return N.ctlCustom(e);
    if (v.valueMissing) return t === 'checkbox' ? 'Please check this box if you want to proceed.' : t === 'radio' ? 'Please select one of these options.' :
        t === 'select' ? 'Please select an item in the list.' : t === 'file' ? 'Please select a file.' : 'Please fill out this field.';
    if (v.typeMismatch) return t === 'email' ? 'Please enter an email address.' : 'Please enter a URL.';
    if (v.patternMismatch) return e.getAttribute('title') || 'Please match the requested format.';
    if (v.tooLong) return 'Please shorten this text.';
    if (v.tooShort) return 'Please lengthen this text.';
    if (v.rangeUnderflow) return 'Value is too small.';
    if (v.rangeOverflow) return 'Value is too large.';
    return v.valid ? '' : 'Please enter a valid value.';
}

/* form */
const formElementsOf = (f) => {
    const out = [];
    const root = f.isConnected ? doc : f;
    for (const e of N.collect(root, 5)) if (formOwner(e) === f) out.push(e);
    return out;
};
defProps(HTMLFormElement.prototype, {
    get elements() { return makeList(HTMLFormControlsCollection, this, -1, null, formElementsOf(this)); },
    get length() { return formElementsOf(this).length; },
    get action() { const v = this.getAttribute('action'); return v ? resolveUrl(v) : N.docUrl(0); },
    set action(v) { this.setAttribute('action', str(v)); },
    get encoding() { return this.enctype; },
    set encoding(v) { this.enctype = v; },
    submit() { submitForm(this, null, false); },
    requestSubmit(submitter) {
        if (submitter != null) {
            if (!isFormControl(submitter) || formOwner(submitter) !== this || !isSubmitButton(submitter))
                throw new TypeError('requestSubmit: the submitter is not a submit button of this form');
        }
        submitForm(this, submitter || null, true);
    },
    reset() {
        if (!fire(this, new Event('reset', { bubbles: true, cancelable: true }))) return;
        for (const e of formElementsOf(this)) resetControl(e);
    },
    checkValidity() { let ok = true; for (const e of formElementsOf(this)) if (e.checkValidity && !e.checkValidity()) ok = false; return ok; },
    reportValidity() { return reportFormValidity(this); },
    item(i) { return formElementsOf(this)[i >>> 0] || null; },
    namedItem(n) { return namedControls(formElementsOf(this), n); },
});

/* Legacy named access: form.controlName and form[index] (past names and
 * image buttons aside), and document.formName for forms, images, embeds
 * and objects with a name. Real properties win: the lookup sits in the
 * prototype chain, behind the interfaces' own members. */
{
    const formNamed = (form, p) => {
        if (isIndex(p)) return formElementsOf(form)[+p];
        const els = formElementsOf(form).filter((e) => !(e.localName === 'input' && inputType(e) === 'image'));
        const v = namedControls(els, p);
        if (v) return v;
        for (const img of form.getElementsByTagName('img'))
            if (img.id === p || img.getAttribute('name') === p) return img;
        return undefined;
    };
    const docNamed = (d, p) => {
        if (!d.documentElement) return undefined;
        const m = [];
        for (const e of d.querySelectorAll('form[name],img[name],embed[name],object[name],object[id],iframe[name]')) {
            const n = e.localName;
            if (e.getAttribute('name') === p || (e.id === p && (n === 'object' || (n === 'img' && e.hasAttribute('name'))))) m.push(e);
        }
        return m.length === 0 ? undefined : m.length === 1 ? m[0] : makeList(HTMLCollection, null, -1, null, m);
    };
    /* One proxy between Node.prototype and EventTarget.prototype: it sees
     * only node lookups that every interface below it missed. Its target
     * is an empty object over EventTarget.prototype, so instanceof still
     * finds every interface; the window's chain does not pass through it,
     * so unresolvable globals still throw. */
    const base = Object.create(Object.getPrototypeOf(Node.prototype));
    Object.setPrototypeOf(Node.prototype, new Proxy(base, {
        get(t, p, r) {
            if (typeof p === 'string' && p !== 'then' && !(p in t) && isNode(r)) {
                let v;
                if (r.nodeType === 9) v = docNamed(r, p);
                else if (r instanceof HTMLFormElement) v = formNamed(r, p);
                if (v !== undefined) return v;
            }
            return Reflect.get(t, p, r);
        },
    }));
    /* The window's named properties (window.foo and a bare foo): an
     * element with that id, or an embed, form, img or object with that
     * name. A proxy between Window.prototype and EventTarget.prototype,
     * as the HTML Standard's named properties object: globals the page
     * defines and the window's own members are found first, and has()
     * answers true only for a match, so an unknown global still throws
     * ReferenceError. Old pages and inline handlers use these. */
    const namedTags = new Set(['embed', 'form', 'img', 'object']);
    const winNamed = (p) => {
        if (typeof p !== 'string' || p === '' || p === 'then' || isIndex(p) || P.namedOff || !doc.documentElement)
            return undefined;
        const m = [];
        const byId = doc.getElementById(p);
        if (byId) m.push(byId);
        for (const e of doc.getElementsByName(p))
            if (e !== byId && e.namespaceURI === HTML_NS && namedTags.has(e.localName)) m.push(e);
        if (m.length > 1) {
            const all = new Set(m);
            const ordered = doc.querySelectorAllArray('*').filter((e) => all.has(e));
            return makeList(HTMLCollection, null, -1, null, ordered);
        }
        return m[0];
    };
    const wbase = Object.create(Object.getPrototypeOf(Window.prototype));
    Object.setPrototypeOf(Window.prototype, new Proxy(wbase, {
        has(t, p) { return p in t || winNamed(p) !== undefined; },
        get(t, p, r) {
            if (!(p in t)) {
                const v = winNamed(p);
                if (v !== undefined) return v;
            }
            return Reflect.get(t, p, r);
        },
    }));
}
const isSubmitButton = (e) => (e.localName === 'button' && (e.getAttribute('type') || 'submit').toLowerCase() === 'submit') ||
    (e.localName === 'input' && (inputType(e) === 'submit' || inputType(e) === 'image'));
function resetControl(e) {
    ctrl.delete(e);
    N.ctlReset(e);
    if (e.localName === 'select') for (const o of selectOptions(e)) { ctrl.delete(o); N.ctlReset(o); }
}
/* Interactive validation: invalid at each invalid control; the first
 * whose event was not cancelled gets the focus. */
function reportFormValidity(form) {
    let ok = true, first = null;
    for (const e of formElementsOf(form)) {
        if (!e.willValidate || validityOf(e).valid) continue;
        ok = false;
        if (fire(e, new Event('invalid', { cancelable: true })) && !first) first = e;
    }
    if (first && first.isConnected) first.focus();
    return ok;
}

/* The form data set: [name, value] pairs (files are not supported). */
function formDataSet(form, submitter) {
    const out = [];
    for (const e of formElementsOf(form)) {
        if (isDisabled(e) || e.localName === 'object' || e.localName === 'fieldset' || e.localName === 'output') continue;
        const t = e.localName === 'input' ? inputType(e) : e.localName;
        if ((t === 'submit' || t === 'button' || t === 'reset' || t === 'image' || e.localName === 'button') && e !== submitter) continue;
        if ((t === 'checkbox' || t === 'radio') && !e.checked) continue;
        const name = e.getAttribute('name');
        if (t === 'image') { const p = name ? name + '.' : ''; out.push([p + 'x', '0'], [p + 'y', '0']); continue; }
        if (!name) continue;
        if (e.localName === 'select') { for (const o of selectOptions(e)) if (o.selected && !o.disabled) out.push([name, o.value]); continue; }
        if (t === 'file') { out.push([name, '']); continue; }
        out.push([name, e.value]);
        const dir = e.getAttribute('dirname');
        if (dir && (t === 'text' || t === 'search' || e.localName === 'textarea')) out.push([dir, 'ltr']);
    }
    return out;
}
let submitting = null;
/* Submits form: with events (requestSubmit, a submit button), interactive
 * validation and a cancelable submit event first. The embedder builds the
 * entry list, encodes it and navigates (src/page/forms.c). */
function submitForm(form, submitter, events) {
    if (!form.isConnected || submitting === form) return;
    if (events) {
        if (!form.noValidate && !(submitter && submitter.hasAttribute('formnovalidate')) && !reportFormValidity(form)) return;
        const ev = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter });
        if (!fire(form, ev)) return;
    }
    submitting = form;
    try {
        N.submitForm(form, submitter || null);
    } finally {
        submitting = null;
    }
}

/* ---------------- activation ---------------- */

/* Runs a click with activation behavior: the nearest activatable
 * element on the path gets legacy pre-activation (checkbox and radio
 * toggling), the event is dispatched, then activation runs unless
 * cancelled, or the pre-activation is undone. */
function activationTarget(t) {
    for (let e = t; e; e = e.parentElement || (e.parentNode && N.hostOf(e.parentNode))) {
        if (!isElement(e)) break;
        if (e.namespaceURI !== HTML_NS) continue;
        const n = e.localName;
        if ((n === 'a' || n === 'area') && e.hasAttribute('href')) return e;
        if (n === 'input' && !/^(hidden)$/.test(inputType(e))) return e;
        if (n === 'button' || n === 'label' || n === 'summary') return e;
    }
    return null;
}
function activate(target, ev, embedder) {
    let act = activationTarget(target);
    /* Clicks from the embedder leave links to the embedder. */
    if (embedder && act && (act.localName === 'a' || act.localName === 'area')) act = null;
    let undo = null;
    if (act && act.localName === 'input' && !isDisabled(act)) {
        const t = inputType(act);
        if (t === 'checkbox') {
            const was = act.checked, wasInd = act.indeterminate;
            act.checked = !was;
            undo = () => { act.checked = was; ctrlState(act).indeterminate = wasInd; };
        } else if (t === 'radio') {
            const group = radioGroup(act);
            const prev = group.find(isChecked) || null;
            setChecked(act, true);
            undo = () => { if (prev) setChecked(prev, true); else N.setChecked(act, false); };
        }
    }
    const ok = fire(target, ev);
    if (!act) return ok;
    if (!ok) { if (undo) undo(); return ok; }
    runActivation(act, undo !== null, ev);
    return ok;
}
function runActivation(e, toggled, ev) {
    const n = e.localName;
    if (n === 'a' || n === 'area') { followHyperlink(e); return; }
    if (n === 'summary') {
        const d = e.parentElement;
        if (d && d.localName === 'details' && childOfType(d, 'summary') === e) {
            d.toggleAttribute('open');
            fire(d, new ToggleEvent('toggle', { oldState: d.open ? 'closed' : 'open', newState: d.open ? 'open' : 'closed' }));
        }
        return;
    }
    if (n === 'label') {
        const c = e.control;
        if (c && c !== ev.target && !c.contains(ev.target)) c.click();
        return;
    }
    if (isDisabled(e)) return;
    const t = n === 'input' ? inputType(e) : (e.getAttribute('type') || 'submit').toLowerCase();
    if (toggled) {
        if (e.isConnected) {
            fire(e, new Event('input', { bubbles: true, composed: true }));
            fire(e, new Event('change', { bubbles: true }));
        }
        return;
    }
    const form = formOwner(e);
    if (form && (t === 'submit' || t === 'image')) form.requestSubmit(e);
    else if (form && t === 'reset') form.reset();
    const pt = popoverTargetOf(e);
    if (pt && e.isConnected) {
        /* The popover target attribute's activation behaviour. */
        const a = e.popoverTargetAction, showing = isOpenPopover(pt);
        if (showing && a !== 'show') hidePopoverEl(pt, true, true, false);
        else if (!showing && a !== 'hide') showPopoverEl(pt, e, false);
    }
}
function followHyperlink(a) {
    const href = a.getAttribute('href');
    if (href === null) return;
    if (a.hasAttribute('download')) N.print(2, 'Downloads from script clicks are not supported.');
    const target = a.getAttribute('target') || (doc.querySelector('base[target]') || { getAttribute: () => '' }).getAttribute('target') || '';
    const url = /^javascript:/i.test(href.trim()) ? href.trim() : resolveUrl(href);
    if (/^javascript:/i.test(url)) { P.runJavascriptUrl(url); return; }
    if (target && target !== '_self' && target !== '_top' && target !== '_parent') N.openWindow(url, target);
    else G.location.assign(url);
}

/* ---------------- other element interfaces ---------------- */

defProps(HTMLImageElement.prototype, {
    get width() { const r = N.rect(this); return r ? Math.round(r[2]) : (parseInt(this.getAttribute('width'), 10) || 0); },
    set width(v) { this.setAttribute('width', String(v >>> 0)); },
    get height() { const r = N.rect(this); return r ? Math.round(r[3]) : (parseInt(this.getAttribute('height'), 10) || 0); },
    set height(v) { this.setAttribute('height', String(v >>> 0)); },
    /* From script's own load of src, else from the image the document
     * loaded for it (N.imgNatural): lazy loaders and galleries test
     * naturalWidth to tell a loaded image. */
    get naturalWidth() { if (this._isz) return this._isz[0]; const n = this.isConnected && N.imgNatural(this); return n ? n[0] : 0; },
    get naturalHeight() { if (this._isz) return this._isz[1]; const n = this.isConnected && N.imgNatural(this); return n ? n[1] : 0; },
    get complete() { return !this._iload; },
    get currentSrc() { return this.src; },
    get x() { const r = N.rect(this); return r ? r[0] : 0; },
    get y() { const r = N.rect(this); return r ? r[1] : 0; },
    /* Resolves once the image has loaded, rejects when it fails. An image
     * without a box is not loaded by the document view (known-gaps.md),
     * so decode() resolves for it rather than wait for ever. */
    decode() {
        return new Promise((resolve, reject) => {
            const fail = () => reject(domErr('The image could not be decoded', 'EncodingError'));
            const wait = () => {
                const off = () => { this.removeEventListener('load', ok); this.removeEventListener('error', bad); };
                const ok = () => { off(); resolve(); };
                const bad = () => { off(); fail(); };
                this.addEventListener('load', ok);
                this.addEventListener('error', bad);
            };
            if (this._iload) { wait(); return; }
            if (this.naturalWidth > 0) { resolve(); return; }
            if (!this.hasAttribute('src') && !this.hasAttribute('srcset')) { fail(); return; }
            if (this.isConnected && N.imgDone(this)) { fail(); return; }
            if (this.isConnected && N.rect(this)) { wait(); return; }
            resolve();
        });
    },
});
/* Setting src from script loads the image here: its header gives
 * naturalWidth and naturalHeight, then load (or error) fires. Images the
 * parser made, or whose src or srcset attribute script set, load through
 * the document view, which fires their events once layout has found the
 * image loaded or failed (fire_image_events in docview.c). */
const imgBytes = { total: 0, registry: new FinalizationRegistry((n) => { imgBytes.total -= n; }) };
{
    const d = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'src');
    Object.defineProperty(HTMLImageElement.prototype, 'src', {
        get: d.get,
        set(v) {
            d.set.call(this, v);
            N.imgClaim(this);
            const img = this, url = this.src;
            if (img._iabort) img._iabort();
            img._isz = null;
            if (img._ibmp || img._ibytes) { img._ibmp = null; img._ibytes = null; }
            if (!url) { img._iload = false; return; }
            img._iload = true;
            const chunks = [];
            let status = 0;
            const fail = () => { img._iload = false; img.dispatchEvent(new Event('error')); };
            /* credentials same-origin (1), mode no-cors (1), dest image (3) */
            try { img._iabort = startFetch('GET', url, [], null, 1, 1, 3, {
                head(s) { status = s; },
                data(b) { chunks.push(b); },
                done(ok) {
                    img._iabort = null;
                    img._iload = false;
                    let size = null;
                    if (ok && status >= 200 && status < 300) {
                        let n = 0;
                        for (const c of chunks) n += c.length;
                        const all = new Uint8Array(n);
                        n = 0;
                        for (const c of chunks) { all.set(c, n); n += c.length; }
                        size = N.imageSize(all);
                        /* The bytes stay with the image, for drawImage and
                         * createImageBitmap to decode (natives_canvas.c
                         * drops them once decoded): at most 4 MB each and
                         * 12 MB over the images still alive. */
                        if (size && n <= 4194304 && imgBytes.total + n <= 12582912) {
                            imgBytes.total += n;
                            imgBytes.registry.register(img, n);
                            img._ibytes = all;
                        }
                    }
                    img._isz = size;
                    img.dispatchEvent(new Event(size ? 'load' : 'error'));
                },
            }); } catch (e) { img._iabort = null; N.queueTask(fail); }
        },
        configurable: true,
        enumerable: true,
    });
}
function Image(w, h) {
    const e = doc.createElement('img');
    if (w !== undefined) e.setAttribute('width', String(w >>> 0));
    if (h !== undefined) e.setAttribute('height', String(h >>> 0));
    return e;
}
Image.prototype = HTMLImageElement.prototype;
defGlobal('Image', Image);
function Option(text, value, defaultSelected, selected) {
    const o = doc.createElement('option');
    if (text !== undefined && text !== '') o.textContent = str(text);
    if (value !== undefined) o.setAttribute('value', str(value));
    if (defaultSelected) o.setAttribute('selected', '');
    if (selected) setOptionSelected(o, true);
    return o;
}
Option.prototype = HTMLOptionElement.prototype;
defGlobal('Option', Option);
function Audio(src) { const e = doc.createElement('audio'); e.setAttribute('preload', 'auto'); if (src !== undefined) e.setAttribute('src', str(src)); return e; }
Audio.prototype = HTMLAudioElement.prototype;
defGlobal('Audio', Audio);

defProps(HTMLMediaElement.prototype, {
    play() { return Promise.reject(domErr('Media playback is not supported', 'NotSupportedError')); },
    pause() {},
    load() {},
    canPlayType() { return ''; },
    get paused() { return true; },
    get ended() { return false; },
    get currentTime() { return 0; },
    set currentTime(v) {},
    get duration() { return NaN; },
    get volume() { return 1; },
    set volume(v) {},
    get muted() { return false; },
    set muted(v) {},
    get readyState() { return 0; },
    get networkState() { return 3; },
    get error() { return null; },
    get currentSrc() { return ''; },
    get playbackRate() { return 1; },
    set playbackRate(v) {},
});
/* ---------------- the top layer: dialogs and popovers ----------------
 * A modal dialog and a showing popover are in the document's top layer
 * (N.setTopLayer, kind 1 and 2), which C keeps in the order they were
 * added (N.topLayer): they match :modal and :popover-open, and layout
 * paints them above the rest of the page. While a modal dialog is open
 * the rest of the document is inert (N.inert). */

/* The dialog focusing steps: the dialog itself when it has autofocus,
 * else its first focusable descendant with autofocus, else its first
 * focusable descendant; with none, the focus leaves what is now inert. */
function dialogFocus(d) {
    let c = d.hasAttribute('autofocus') && isFocusable(d) ? d : null;
    if (!c) for (const e of d.querySelectorAll('[autofocus]')) if (isFocusable(e)) { c = e; break; }
    if (!c) for (const e of d.querySelectorAll('*')) if (isFocusable(e)) { c = e; break; }
    if (c) c.focus();
    else if (focused && !d.contains(focused)) setFocus(null);
}
defProps(HTMLDialogElement.prototype, {
    show() {
        if (this.hasAttribute('open')) {
            if (this.matches(':modal')) throw domErr('The dialog is open and modal', 'InvalidStateError');
            return;
        }
        this.setAttribute('open', '');
    },
    showModal() {
        if (this.hasAttribute('open')) {
            if (this.matches(':modal')) return;
            throw domErr('The dialog is already open', 'InvalidStateError');
        }
        if (!this.isConnected) throw domErr('The dialog is not connected', 'InvalidStateError');
        if (isOpenPopover(this)) throw domErr('The dialog is a showing popover', 'InvalidStateError');
        this.setAttribute('open', '');
        const st = ctrlState(this);
        st.prevFocus = focused;
        N.setTopLayer(this, 1, true);
        /* Popovers that are not ancestors of the dialog close. */
        hidePopoversUntil(nearestOpenAuto(this.parentElement, autoStack()));
        dialogFocus(this);
    },
    close(rv) {
        if (!this.hasAttribute('open')) return;
        const st = ctrlState(this), prev = st.prevFocus;
        st.prevFocus = null;
        N.setTopLayer(this, 1, false);
        this.removeAttribute('open');
        if (rv !== undefined) st.returnValue = str(rv);
        /* The focus goes back to where it was before showModal(). */
        if (prev && prev.isConnected && (!focused || !focused.isConnected || this.contains(focused))) prev.focus();
        fireSimple(this, 'close', false, false);
    },
    get returnValue() { return (ctrl.get(this) || {}).returnValue || ''; },
    set returnValue(v) { ctrlState(this).returnValue = str(v); },
    requestClose(rv) {
        if (!this.hasAttribute('open')) return;
        if (fire(this, new Event('cancel', { cancelable: true }))) this.close(rv);
    },
});

/* Popovers. 'hint' popovers are kept with the auto ones (one stack). */
function popoverKind(el) {
    const v = isElement(el) && el.namespaceURI === HTML_NS ? el.getAttribute('popover') : null;
    if (v === null) return null;
    const k = v.toLowerCase();
    return k === '' || k === 'auto' ? 'auto' : k === 'hint' ? 'hint' : 'manual';
}
const isOpenPopover = (el) => isElement(el) && el.matches(':popover-open');
/* The showing auto popovers, bottom first. */
const autoStack = () => N.topLayer().filter((e) => isOpenPopover(e) && popoverKind(e) !== 'manual');
/* The nearest inclusive ancestor of n in stack. */
function nearestOpenAuto(n, stack) {
    for (let e = n; e; e = e.parentElement) if (stack.includes(e)) return e;
    return null;
}
/* Of two popovers in stack (either may be null), the higher. */
const higherOf = (stack, a, b) => (!a ? b : !b ? a : stack.indexOf(a) > stack.indexOf(b) ? a : b);
/* The popover a button or input button shows or hides, or null. */
function popoverTargetOf(e) {
    if (!isElement(e) || e.namespaceURI !== HTML_NS) return null;
    if (e.localName === 'input') {
        const t = inputType(e);
        if (t !== 'button' && t !== 'reset' && t !== 'submit' && t !== 'image') return null;
        if ((t === 'submit' || t === 'image') && formOwner(e)) return null;
    } else if (e.localName === 'button') {
        if (e.type === 'submit' && formOwner(e)) return null;
    } else {
        return null;
    }
    if (isDisabled(e)) return null;
    let t = ctrl.get(e) && ctrl.get(e).popoverTarget;
    if (!t || !t.isConnected) {
        const id = e.getAttribute('popovertarget');
        t = id === null || !e.isConnected ? null : e.getRootNode().getElementById(id);
    }
    return t && popoverKind(t) ? t : null;
}
/* Hides the auto popovers above endpoint, the topmost first (all of
 * them when endpoint is null or not showing). */
function hidePopoversUntil(endpoint) {
    for (let guard = 0; guard < 1000; guard++) {
        const stack = autoStack();
        if (!stack.length) return;
        const top = stack[stack.length - 1];
        if (top === endpoint) return;
        if (!hidePopoverEl(top, false, true, false)) return;
    }
}
/* The popover that must stay open when el opens: the highest showing
 * auto popover among el's ancestors and its invoker's. */
function popoverAncestor(el, invoker) {
    const stack = autoStack();
    return higherOf(stack, nearestOpenAuto(el.parentElement, stack), invoker ? nearestOpenAuto(invoker, stack) : null);
}
function queueToggle(el, oldState, newState) {
    const st = ctrlState(el);
    if (st.toggleTask) { st.toggleTask.newState = newState; return; }
    const t = st.toggleTask = { oldState, newState };
    N.queueTask(() => {
        st.toggleTask = null;
        if (t.oldState !== t.newState) fire(el, new ToggleEvent('toggle', { oldState: t.oldState, newState: t.newState }));
    });
}
function showPopoverEl(el, invoker, throws) {
    const fail = (msg, name) => { if (throws) throw domErr(msg, name); return false; };
    const kind = popoverKind(el);
    if (!kind) return fail('The element is not a popover', 'NotSupportedError');
    if (isOpenPopover(el)) return false;
    if (!el.isConnected) return fail('The popover is not connected', 'InvalidStateError');
    if (el.localName === 'dialog' && el.hasAttribute('open')) return fail('The dialog is open', 'InvalidStateError');
    if (!fire(el, new ToggleEvent('beforetoggle', { cancelable: true, oldState: 'closed', newState: 'open' }))) return false;
    const still = () => !isOpenPopover(el) && el.isConnected && popoverKind(el) === kind;
    if (!still()) return false;
    if (kind !== 'manual') {
        hidePopoversUntil(popoverAncestor(el, invoker));
        if (!still()) return false;
    }
    const st = ctrlState(el);
    st.invoker = invoker || null;
    st.prevFocus = focused;
    N.setTopLayer(el, 2, true);
    queueToggle(el, 'closed', 'open');
    /* The popover focusing steps: autofocus only. */
    const af = el.hasAttribute('autofocus') ? el : el.querySelector('[autofocus]');
    if (af && isFocusable(af)) af.focus();
    return true;
}
function hidePopoverEl(el, focusBack, events, throws) {
    if (!popoverKind(el)) {
        if (throws) throw domErr('The element is not a popover', 'NotSupportedError');
        return false;
    }
    if (!isOpenPopover(el)) return false;
    if (popoverKind(el) !== 'manual') {
        hidePopoversUntil(el);
        if (!isOpenPopover(el)) return false;
    }
    const st = ctrlState(el);
    st.invoker = null;
    if (events) {
        fire(el, new ToggleEvent('beforetoggle', { oldState: 'open', newState: 'closed' }));
        if (!isOpenPopover(el)) return false;
    }
    N.setTopLayer(el, 2, false);
    if (events) queueToggle(el, 'open', 'closed');
    const prev = st.prevFocus;
    st.prevFocus = null;
    if (focusBack && prev && prev.isConnected && focused && el.contains(focused)) prev.focus();
    return true;
}
defProps(HEP, {
    get popover() { return popoverKind(this); },
    set popover(v) { if (v === null) this.removeAttribute('popover'); else this.setAttribute('popover', str(v)); },
    showPopover(opts) {
        const src = opts && typeof opts === 'object' && isElement(opts.source) ? opts.source : null;
        showPopoverEl(this, src, true);
    },
    hidePopover() { hidePopoverEl(this, true, true, true); },
    togglePopover(opts) {
        let force, src = null;
        if (typeof opts === 'boolean') force = opts;
        else if (opts && typeof opts === 'object') { force = opts.force; if (isElement(opts.source)) src = opts.source; }
        if (!popoverKind(this)) throw domErr('The element is not a popover', 'NotSupportedError');
        const showing = isOpenPopover(this);
        if (showing && force !== true) hidePopoverEl(this, true, true, true);
        else if (!showing && force !== false) showPopoverEl(this, src, true);
        return isOpenPopover(this);
    },
});
for (const proto of [HTMLButtonElement.prototype, HTMLInputElement.prototype]) {
    defProps(proto, {
        get popoverTargetElement() {
            const t = ctrl.get(this) && ctrl.get(this).popoverTarget;
            if (t && t.isConnected) return t;
            const id = this.getAttribute('popovertarget');
            return id === null || !this.isConnected ? null : this.getRootNode().getElementById(id);
        },
        set popoverTargetElement(v) {
            ctrlState(this).popoverTarget = isElement(v) ? v : null;
            if (v === null) this.removeAttribute('popovertarget');
            else this.setAttribute('popovertarget', '');
        },
    });
}

/* Entry points: Escape (close request) and light dismiss. */
P.closeRequest = function () {
    const top = N.topLayer();
    for (let i = top.length; i-- > 0;) {
        const el = top[i];
        if (el.localName === 'dialog' && el.matches(':modal')) { el.requestClose(); return true; }
        if (isOpenPopover(el) && popoverKind(el) !== 'manual') { hidePopoverEl(el, true, true, false); return true; }
    }
    return false;
};
/* A press (up false) and its release (up true) on target: when both are
 * in the same popover (or outside every one), the auto popovers above it
 * close. A press on a popover's invoker counts as a press in it. */
let popoverDown = null;
function clickedPopover(n, stack) {
    let inv = null;
    for (let e = n; e && !inv; e = e.parentElement) {
        const t = popoverTargetOf(e);
        if (t && stack.includes(t)) inv = t;
    }
    return higherOf(stack, nearestOpenAuto(n, stack), inv);
}
P.lightDismiss = function (target, up) {
    const stack = autoStack();
    if (!stack.length) { popoverDown = null; return; }
    const clicked = clickedPopover(isElement(target) ? target : null, stack);
    if (!up) { popoverDown = clicked; return; }
    const same = clicked === popoverDown;
    popoverDown = null;
    if (same) hidePopoversUntil(clicked);
};
defProps(HTMLScriptElement.prototype, {
    get text() { let s = ''; for (let c = this.firstChild; c; c = c.nextSibling) if (c.nodeType === 3) s += c.data; return s; },
    set text(v) { this.textContent = v; },
});
HTMLScriptElement.supports = (t) => t === 'classic' || t === 'module' || t === 'importmap';
defProps(HTMLTitleElement.prototype, {
    get text() { return this.textContent; },
    set text(v) { this.textContent = v; },
});
/* Nested documents (prelude_window.js: frameWindowOf, frameDocumentOf). */
const frameMembers = {
    get contentDocument() { return frameDocumentOf(this); },
    get contentWindow() { return frameWindowOf(this); },
    getSVGDocument() { return null; },
};
defProps(HTMLIFrameElement.prototype, frameMembers);
defProps(HTMLObjectElement.prototype, frameMembers);
defProps(HTMLFrameElement.prototype, frameMembers);
defProps(HTMLEmbedElement.prototype, { getSVGDocument() { return null; } });
defProps(HTMLSlotElement.prototype, {
    assignedNodes(opts) { return N.assigned(this, !!(opts && opts.flatten)); },
    assignedElements(opts) { return N.assigned(this, !!(opts && opts.flatten)).filter(isElement); },
    assign(...nodes) {
        for (const n of nodes) if (!(n instanceof Element) && !(n instanceof Text)) throw new TypeError('slot.assign takes elements and text nodes');
        N.slotAssign(this, nodes);
    },
});
defProps(HTMLDataListElement.prototype, { get options() { return makeList(HTMLCollection, this, 6); } });
defProps(HTMLMapElement.prototype, { get areas() { return makeList(HTMLCollection, this, 2, 'area'); } });
defProps(HTMLTimeElement.prototype, {});

/* tables */
const tagIs = (n, t) => n && n.nodeType === 1 && n.localName === t && n.namespaceURI === HTML_NS;
const childOfType = (p, t) => { for (let c = p.firstElementChild; c; c = c.nextElementSibling) if (tagIs(c, t)) return c; return null; };
defProps(HTMLTableElement.prototype, {
    get caption() { return childOfType(this, 'caption'); },
    set caption(v) { const c = this.caption; if (c) c.remove(); if (v) this.insertBefore(v, this.firstChild); },
    get tHead() { return childOfType(this, 'thead'); },
    set tHead(v) {
        if (v && !tagIs(v, 'thead')) throw domErr('Not a thead', 'HierarchyRequestError');
        const c = this.tHead; if (c) c.remove();
        if (v) { let r = this.firstChild; while (r && (tagIs(r, 'caption') || tagIs(r, 'colgroup') || r.nodeType !== 1)) r = r.nextSibling; this.insertBefore(v, r); }
    },
    get tFoot() { return childOfType(this, 'tfoot'); },
    set tFoot(v) {
        if (v && !tagIs(v, 'tfoot')) throw domErr('Not a tfoot', 'HierarchyRequestError');
        const c = this.tFoot; if (c) c.remove(); if (v) this.appendChild(v);
    },
    get tBodies() { return cachedList(this, 'tBodies', () => makeList(HTMLCollection, this, 9)); },
    get rows() { return cachedList(this, 'rows', () => makeList(HTMLCollection, this, 7)); },
    createCaption() { return this.caption || this.insertBefore(doc.createElement('caption'), this.firstChild); },
    deleteCaption() { const c = this.caption; if (c) c.remove(); },
    createTHead() { if (this.tHead) return this.tHead; const h = doc.createElement('thead'); this.tHead = h; return h; },
    deleteTHead() { const c = this.tHead; if (c) c.remove(); },
    createTFoot() { if (this.tFoot) return this.tFoot; const f = doc.createElement('tfoot'); this.appendChild(f); return f; },
    deleteTFoot() { const c = this.tFoot; if (c) c.remove(); },
    createTBody() {
        const b = doc.createElement('tbody');
        const bodies = this.tBodies;
        const last = bodies[bodies.length - 1];
        this.insertBefore(b, last ? last.nextSibling : null);
        return b;
    },
    insertRow(i = -1) {
        i |= 0;
        const rows = N.collect(this, 7);
        if (i < -1 || i > rows.length) throw domErr('Row index out of range', 'IndexSizeError');
        const r = doc.createElement('tr');
        if (rows.length === 0) {
            const bodies = N.collect(this, 9);
            if (bodies.length) bodies[bodies.length - 1].appendChild(r);
            else this.appendChild(doc.createElement('tbody')).appendChild(r);
        } else if (i === -1 || i === rows.length) {
            rows[rows.length - 1].parentNode.appendChild(r);
        } else {
            rows[i].parentNode.insertBefore(r, rows[i]);
        }
        return r;
    },
    deleteRow(i) {
        i |= 0;
        const rows = N.collect(this, 7);
        if (i === -1) { if (rows.length) rows[rows.length - 1].remove(); return; }
        if (i < 0 || i >= rows.length) throw domErr('Row index out of range', 'IndexSizeError');
        rows[i].remove();
    },
});
defProps(HTMLTableSectionElement.prototype, {
    get rows() { return makeList(HTMLCollection, this, 2, 'tr'); },
    insertRow(i = -1) {
        i |= 0;
        const rows = Array.from(this.children).filter((c) => tagIs(c, 'tr'));
        if (i < -1 || i > rows.length) throw domErr('Row index out of range', 'IndexSizeError');
        const r = doc.createElement('tr');
        this.insertBefore(r, i === -1 || i === rows.length ? null : rows[i]);
        return r;
    },
    deleteRow(i) {
        i |= 0;
        const rows = Array.from(this.children).filter((c) => tagIs(c, 'tr'));
        if (i === -1) { if (rows.length) rows[rows.length - 1].remove(); return; }
        if (i < 0 || i >= rows.length) throw domErr('Row index out of range', 'IndexSizeError');
        rows[i].remove();
    },
});
defProps(HTMLTableRowElement.prototype, {
    get cells() { return cachedList(this, 'cells', () => makeList(HTMLCollection, this, 8)); },
    get rowIndex() { const t = this.closest('table'); return t ? N.collect(t, 7).indexOf(this) : -1; },
    get sectionRowIndex() { const p = this.parentNode; return p ? Array.from(p.children).filter((c) => tagIs(c, 'tr')).indexOf(this) : -1; },
    insertCell(i = -1) {
        i |= 0;
        const cells = N.collect(this, 8);
        if (i < -1 || i > cells.length) throw domErr('Cell index out of range', 'IndexSizeError');
        const c = doc.createElement('td');
        this.insertBefore(c, i === -1 || i === cells.length ? null : cells[i]);
        return c;
    },
    deleteCell(i) {
        i |= 0;
        const cells = N.collect(this, 8);
        if (i === -1) { if (cells.length) cells[cells.length - 1].remove(); return; }
        if (i < 0 || i >= cells.length) throw domErr('Cell index out of range', 'IndexSizeError');
        cells[i].remove();
    },
});
defProps(HTMLTableCellElement.prototype, {
    get cellIndex() { const p = this.parentNode; return p && tagIs(p, 'tr') ? N.collect(p, 8).indexOf(this) : -1; },
});

/* ---------------- Document members ---------------- */

let readyState = 'loading';
defProps(DP, {
    get readyState() { return readyState; },
    get location() { return this === doc ? G.location : null; },
    set location(v) { if (this === doc) G.location.href = v; },
    get defaultView() { return this === doc ? G : null; },
    get currentScript() { return this === doc ? N.currentScript() : null; },
    get cookie() { return this === doc ? N.getCookie() : ''; },
    set cookie(v) { if (this === doc) N.setCookie(str(v)); },
    get referrer() { return this === doc ? N.docUrl(3) : ''; },
    get domain() { try { return new URL(N.docUrl(0)).hostname; } catch (e) { return ''; } },
    set domain(v) { throw domErr('Setting document.domain is not supported', 'SecurityError'); },
    get lastModified() {
        const d = new Date();
        const p = (n) => String(n).padStart(2, '0');
        return p(d.getMonth() + 1) + '/' + p(d.getDate()) + '/' + d.getFullYear() + ' ' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
    },
    get characterSet() { return 'UTF-8'; },
    get charset() { return 'UTF-8'; },
    get inputEncoding() { return 'UTF-8'; },
    get contentType() { return 'text/html'; },
    get hidden() { return false; },
    get visibilityState() { return 'visible'; },
    get designMode() { return 'off'; },
    set designMode(v) {},
    get dir() { const h = this.documentElement; return h && h.localName === 'html' ? h.dir : ''; },
    set dir(v) { const h = this.documentElement; if (h && h.localName === 'html') h.dir = v; },
    get scrollingElement() { return this.compatMode === 'BackCompat' ? this.body : this.documentElement; },
    get fullscreenElement() { return null; },
    get fullscreenEnabled() { return false; },
    get pictureInPictureEnabled() { return false; },
    get images() { return cachedList(this, 'images', () => makeList(HTMLCollection, this, 10)); },
    get embeds() { return cachedList(this, 'embeds', () => makeList(HTMLCollection, this, 2, 'embed')); },
    get plugins() { return this.embeds; },
    get links() { return cachedList(this, 'links', () => makeList(HTMLCollection, this, 11)); },
    get forms() { return cachedList(this, 'forms', () => makeList(HTMLCollection, this, 12)); },
    get scripts() { return cachedList(this, 'scripts', () => makeList(HTMLCollection, this, 13)); },
    get anchors() { return cachedList(this, 'anchors', () => makeList(HTMLCollection, this, 14)); },
    get applets() { return makeList(HTMLCollection, this, -1, null, []); },
    get all() { return cachedList(this, 'all', () => makeList(HTMLCollection, this, 2, '*')); },
    getElementsByName(name) { return cachedList(this, 'n:' + str(name), () => makeList(NodeList, this, 4, str(name))); },
    createEvent(iface) {
        const map = { event: Event, events: Event, htmlevents: Event, customevent: CustomEvent, uievent: UIEvent,
            uievents: UIEvent, mouseevent: MouseEvent, mouseevents: MouseEvent, keyboardevent: KeyboardEvent,
            focusevent: FocusEvent, messageevent: MessageEvent, storageevent: StorageEvent, hashchangeevent: HashChangeEvent,
            errorevent: ErrorEvent, popstateevent: PopStateEvent, progressevent: ProgressEvent, compositionevent: CompositionEvent,
            beforeunloadevent: BeforeUnloadEvent, pagetransitionevent: PageTransitionEvent, wheelevent: WheelEvent,
            pointerevent: PointerEvent, inputevent: InputEvent };
        const C = map[str(iface).toLowerCase()];
        if (!C) throw domErr('The event interface is not supported', 'NotSupportedError');
        const e = new C('');
        e[S.ev].initialized = false;
        return e;
    },
    createRange() { return new (ranges().Range)(); }, /* prelude_lazy.js */
    createNodeIterator(root, what, filter) { return new NodeIterator(root, what, filter); },
    createTreeWalker(root, what, filter) { return new TreeWalker(root, what, filter); },
    /* document.open() and close(): during parsing, write() inserts at the
     * parser's insertion point and open() has nothing to do; after
     * parsing, write() itself replaces the document (natives.c). */
    open() { if (arguments.length > 2) return G.open.apply(G, arguments); N.docOpen(this, false); return this; },
    close() { N.docOpen(this, true); },
    execCommand() { return false; },
    queryCommandSupported() { return false; },
    queryCommandEnabled() { return false; },
    queryCommandState() { return false; },
    queryCommandValue() { return ''; },
    getSelection() { return G.getSelection(); },
    /* the element under the point, retargeted to this document's tree (a
     * node in a shadow tree reports its host) */
    elementFromPoint(x, y) {
        if (this !== doc) return null;
        const e = N.hitTest(+x, +y);
        return e ? retarget(e, this) : null;
    },
    elementsFromPoint(x, y) {
        const e = this.elementFromPoint(x, y);
        const out = [];
        for (let n = e; n; n = n.parentNode) if (isElement(n)) out.push(n);
        return out;
    },
    exitFullscreen() { return Promise.reject(new TypeError('Fullscreen is not supported')); },
    clear() {},
    captureEvents() {},
    releaseEvents() {},
    get fgColor() { const b = this.body; return b ? b.getAttribute('text') || '' : ''; },
    get bgColor() { const b = this.body; return b ? b.getAttribute('bgcolor') || '' : ''; },
    get linkColor() { const b = this.body; return b ? b.getAttribute('link') || '' : ''; },
});
/* The document lifecycle: "interactive" fires readystatechange and
 * DOMContentLoaded; "complete" fires readystatechange, load (at window,
 * with the document as target as browsers do) and pageshow. */
let contentLoaded = false;
P.setReadyState = function (s) {
    if (s === 'contentloaded') {
        if (contentLoaded) return;
        contentLoaded = true;
        if (readyState === 'loading') P.setReadyState('interactive');
        fireSimple(doc, 'DOMContentLoaded', true, false);
        return;
    }
    if (s === readyState) return;
    if (s === 'complete' && !contentLoaded) P.setReadyState('contentloaded');
    readyState = s;
    fireSimple(doc, 'readystatechange', false, false);
    if (s === 'complete') {
        fire(G, new Event('load'), doc);
        fire(G, new PageTransitionEvent('pageshow', { persisted: false }), doc);
    }
};

/* ---------------- traversal ---------------- */

const NodeFilter = {
    FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 0x1, SHOW_ATTRIBUTE: 0x2,
    SHOW_TEXT: 0x4, SHOW_CDATA_SECTION: 0x8, SHOW_ENTITY_REFERENCE: 0x10, SHOW_ENTITY: 0x20,
    SHOW_PROCESSING_INSTRUCTION: 0x40, SHOW_COMMENT: 0x80, SHOW_DOCUMENT: 0x100, SHOW_DOCUMENT_TYPE: 0x200,
    SHOW_DOCUMENT_FRAGMENT: 0x400, SHOW_NOTATION: 0x800,
};
defGlobal('NodeFilter', NodeFilter);
function filterNode(it, n) {
    if (it._active) throw domErr('The filter is already running', 'InvalidStateError');
    if (!((1 << (n.nodeType - 1)) & it.whatToShow)) return 3;
    const f = it.filter;
    if (f === null) return 1;
    it._active = true;
    try {
        const r = typeof f === 'function' ? f.call(undefined, n) : f.acceptNode(n);
        return Number(r) | 0;
    } finally {
        it._active = false;
    }
}
class TreeWalker {
    constructor(root, what = 0xFFFFFFFF, filter = null) {
        if (!isNode(root)) throw new TypeError('createTreeWalker: root is not a Node');
        this._root = root; this.currentNode = root; this._what = what >>> 0; this._filter = filter; this._active = false;
    }
    get root() { return this._root; }
    get whatToShow() { return this._what; }
    get filter() { return this._filter; }
    parentNode() {
        let n = this.currentNode;
        while (n && n !== this._root) {
            n = n.parentNode;
            if (n && filterNode(this, n) === 1) return (this.currentNode = n);
        }
        return null;
    }
    _child(first) {
        let n = first ? this.currentNode.firstChild : this.currentNode.lastChild;
        while (n) {
            const r = filterNode(this, n);
            if (r === 1) return (this.currentNode = n);
            if (r === 3) { const c = first ? n.firstChild : n.lastChild; if (c) { n = c; continue; } }
            while (n) {
                const s = first ? n.nextSibling : n.previousSibling;
                if (s) { n = s; break; }
                const p = n.parentNode;
                if (!p || p === this._root || p === this.currentNode) return null;
                n = p;
            }
        }
        return null;
    }
    firstChild() { return this._child(true); }
    lastChild() { return this._child(false); }
    _sibling(next) {
        let n = this.currentNode;
        if (n === this._root) return null;
        for (;;) {
            let s = next ? n.nextSibling : n.previousSibling;
            while (s) {
                n = s;
                const r = filterNode(this, n);
                if (r === 1) return (this.currentNode = n);
                s = next ? n.firstChild : n.lastChild;
                if (r === 2 || !s) s = next ? n.nextSibling : n.previousSibling;
            }
            n = n.parentNode;
            if (!n || n === this._root) return null;
            if (filterNode(this, n) === 1) return null;
        }
    }
    nextSibling() { return this._sibling(true); }
    previousSibling() { return this._sibling(false); }
    previousNode() {
        let n = this.currentNode;
        while (n !== this._root) {
            let s = n.previousSibling;
            while (s) {
                n = s;
                let r = filterNode(this, n);
                while (r !== 2 && n.lastChild) { n = n.lastChild; r = filterNode(this, n); }
                if (r === 1) return (this.currentNode = n);
                s = n.previousSibling;
            }
            if (n === this._root || !n.parentNode) return null;
            n = n.parentNode;
            if (filterNode(this, n) === 1) return (this.currentNode = n);
        }
        return null;
    }
    nextNode() {
        let n = this.currentNode, r = 1;
        for (;;) {
            while (r !== 2 && n.firstChild) {
                n = n.firstChild;
                r = filterNode(this, n);
                if (r === 1) return (this.currentNode = n);
            }
            let s = null;
            for (let t = n; t; t = t.parentNode) {
                if (t === this._root) return null;
                if (t.nextSibling) { s = t.nextSibling; break; }
            }
            if (!s) return null;
            n = s;
            r = filterNode(this, n);
            if (r === 1) return (this.currentNode = n);
        }
    }
}
tag(TreeWalker);
defGlobal('TreeWalker', TreeWalker);
class NodeIterator {
    constructor(root, what = 0xFFFFFFFF, filter = null) {
        if (!isNode(root)) throw new TypeError('createNodeIterator: root is not a Node');
        this._root = root; this._ref = root; this._before = true; this._what = what >>> 0; this._filter = filter; this._active = false;
    }
    get root() { return this._root; }
    get referenceNode() { return this._ref; }
    get pointerBeforeReferenceNode() { return this._before; }
    get whatToShow() { return this._what; }
    get filter() { return this._filter; }
    _traverse(next) {
        let n = this._ref, before = this._before;
        for (;;) {
            if (next) {
                if (!before) {
                    let m = n.firstChild;
                    if (!m) for (let t = n; t && t !== this._root; t = t.parentNode) if (t.nextSibling) { m = t.nextSibling; break; }
                    if (!m) return null;
                    n = m;
                }
                before = false;
            } else {
                if (before) {
                    if (n === this._root) return null;
                    let m = n.previousSibling;
                    if (m) { while (m.lastChild) m = m.lastChild; } else m = n.parentNode;
                    if (!m) return null;
                    n = m;
                }
                before = true;
            }
            if (filterNode(this, n) === 1) { this._ref = n; this._before = before; return n; }
            if (!next && !before) before = true;
        }
    }
    nextNode() { return this._traverse(true); }
    previousNode() { return this._traverse(false); }
    detach() {}
}
tag(NodeIterator);
defGlobal('NodeIterator', NodeIterator);

/* DOMParser, XMLSerializer, DOMImplementation */
class XMLSerializer {
    serializeToString(n) {
        if (!isNode(n)) throw new TypeError('serializeToString: not a Node');
        if (isElement(n)) return n.outerHTML;
        if (n.nodeType === 3) return n.data.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        if (n.nodeType === 8) return '<!--' + n.data + '-->';
        const d = doc.createElement('div');
        d.appendChild(n.cloneNode(true));
        return d.innerHTML;
    }
}
defGlobal('XMLSerializer', XMLSerializer);
class DOMParser {
    parseFromString(text, type) {
        type = str(type);
        if (type !== 'text/html') {
            if (['text/xml', 'application/xml', 'application/xhtml+xml', 'image/svg+xml'].includes(type))
                throw domErr('DOMParser: XML documents are not supported', 'NotSupportedError');
            throw new TypeError('DOMParser: unknown type ' + type);
        }
        return N.parseHTMLDocument(str(text));
    }
}
defGlobal('DOMParser', DOMParser);
/* Document.parseHTMLUnsafe: DOMParser's document with declarative shadow
 * roots. */
Object.defineProperty(Document, 'parseHTMLUnsafe', {
    value: function parseHTMLUnsafe(html) { return N.parseHTMLDocument(str(html), true); },
    configurable: true, writable: true,
});
class DOMImplementation {
    hasFeature() { return true; }
    createHTMLDocument(title) {
        const d = N.parseHTMLDocument('<!DOCTYPE html><html><head></head><body></body></html>');
        if (title !== undefined) {
            const t = d.createElement('title');
            t.textContent = str(title);
            d.head.appendChild(t);
        }
        return d;
    }
    /* An empty document with the root element and doctype asked for. It
     * is a plain document: nothing renders it and no script runs in it. */
    createDocument(ns, qname, doctype) {
        const d = N.newDocument();
        if (doctype !== undefined && doctype !== null) {
            if (!isNode(doctype) || doctype.nodeType !== 10) throw new TypeError('createDocument: the doctype is not a DocumentType');
            d.appendChild(doctype);
        }
        if (qname !== undefined && qname !== null && str(qname) !== '') d.appendChild(d.createElementNS(ns === undefined ? null : ns, qname));
        return d;
    }
    createDocumentType(name, publicId, systemId) {
        if (arguments.length < 3) throw new TypeError('createDocumentType: 3 arguments required');
        name = str(name);
        if (name === '' || /[\s>]/.test(name) || /^[-.0-9]/.test(name)) throw domErr("'" + name + "' is not a valid name", 'InvalidCharacterError');
        return N.newDoctype(name, str(publicId), str(systemId));
    }
}
tag(DOMImplementation);
defGlobal('DOMImplementation', DOMImplementation);
const theImpl = Object.create(DOMImplementation.prototype);
defProps(DP, {
    get implementation() { return theImpl; },
    /* HTML documents have no CDATA sections, as the standard says */
    createCDATASection() { throw domErr('This is an HTML document', 'NotSupportedError'); },
});

/* ARIA attributes as properties (ARIAMixin): the attribute's value, or
 * null when it is absent. */
for (const name of ['role', 'ariaAtomic', 'ariaAutoComplete', 'ariaBusy', 'ariaChecked', 'ariaColCount', 'ariaColIndex', 'ariaColSpan',
    'ariaCurrent', 'ariaDescription', 'ariaDisabled', 'ariaExpanded', 'ariaHasPopup', 'ariaHidden', 'ariaInvalid', 'ariaKeyShortcuts',
    'ariaLabel', 'ariaLevel', 'ariaLive', 'ariaModal', 'ariaMultiLine', 'ariaMultiSelectable', 'ariaOrientation', 'ariaPlaceholder',
    'ariaPosInSet', 'ariaPressed', 'ariaReadOnly', 'ariaRequired', 'ariaRoleDescription', 'ariaRowCount', 'ariaRowIndex', 'ariaRowSpan',
    'ariaSelected', 'ariaSetSize', 'ariaSort', 'ariaValueMax', 'ariaValueMin', 'ariaValueNow', 'ariaValueText']) {
    const attr = name === 'role' ? 'role' : 'aria-' + name.slice(4).toLowerCase();
    Object.defineProperty(EP, name, {
        get() { return this.getAttribute(attr); },
        set(v) { if (v === null || v === undefined) this.removeAttribute(attr); else this.setAttribute(attr, str(v)); },
        configurable: true, enumerable: true,
    });
}
