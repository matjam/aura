/* Aura's URLPattern (WHATWG URL Pattern). The build compiles this file
 * to its own bytecode (urlpattern.bc); the prelude reads it with
 * N.lazy(4) the first time script touches URLPattern. It is a function
 * of G and H and returns the class.
 *
 * Each component's pattern is tokenized and parsed as the standard says
 * (fixed text, :name groups, (regexp) groups, * wildcards, {} groups,
 * and the ?, + and * modifiers) and compiled to a RegExp. A pattern
 * given as one string is split into components by scanning for its
 * delimiters outside groups, a simplification of the standard's
 * constructor string parser. docs/javascript.md ("URLPattern") has the
 * details. */
(function (G, H) {
'use strict';

const COMPONENTS = ['protocol', 'username', 'password', 'hostname', 'port', 'pathname', 'search', 'hash'];
const SPECIAL = { 'ftp:': 21, 'file:': null, 'http:': 80, 'https:': 443, 'ws:': 80, 'wss:': 443 };

/* ---------------- Tokenizer ---------------- */

const nameStart = /[$_\p{ID_Start}]/u, namePart = /[$_‌‍\p{ID_Continue}]/u;

function tokenize(input) {
    const toks = [];
    let i = 0;
    const err = (m) => { throw new TypeError("Invalid pattern '" + input + "': " + m); };
    while (i < input.length) {
        const c = input[i];
        if (c === '*') { toks.push({ t: 'asterisk', v: c, i }); i++; continue; }
        if (c === '+' || c === '?') { toks.push({ t: 'modifier', v: c, i }); i++; continue; }
        if (c === '\\') {
            if (i + 1 >= input.length) err('a backslash at the end');
            toks.push({ t: 'escaped', v: input[i + 1], i });
            i += 2;
            continue;
        }
        if (c === '{') { toks.push({ t: 'open', v: c, i }); i++; continue; }
        if (c === '}') { toks.push({ t: 'close', v: c, i }); i++; continue; }
        if (c === ':') {
            let j = i + 1;
            while (j < input.length && (j === i + 1 ? nameStart : namePart).test(input[j])) j++;
            if (j === i + 1) err('a group name is missing');
            toks.push({ t: 'name', v: input.slice(i + 1, j), i });
            i = j;
            continue;
        }
        if (c === '(') {
            let depth = 1, j = i + 1, re = '';
            if (input[j] === '?') err('a regexp group may not start with ?');
            while (j < input.length) {
                const d = input[j];
                if (d === '\\') { re += d + (input[j + 1] || ''); j += 2; continue; }
                if (d === ')') { if (--depth === 0) { j++; break; } }
                else if (d === '(') {
                    depth++;
                    if (input[j + 1] !== '?') err('a capturing group inside a regexp group');
                }
                re += d;
                j++;
            }
            if (depth) err('an unclosed regexp group');
            if (!re) err('an empty regexp group');
            toks.push({ t: 'regexp', v: re, i });
            i = j;
            continue;
        }
        toks.push({ t: 'char', v: c, i });
        i++;
    }
    toks.push({ t: 'end', v: '', i });
    return toks;
}

/* ---------------- Parser ---------------- */

const escapeRe = (s) => s.replace(/[.+*?^${}()[\]|/\\]/g, '\\$&');
const escapePat = (s) => s.replace(/[+*?:{}()\\]/g, '\\$&');

/* Parts: { type: 'fixed' | 'regexp' | 'segment' | 'full', value, name, prefix, suffix, modifier } */
function parse(input, segRe, prefixChars) {
    const toks = tokenize(input);
    const parts = [];
    let pending = '', k = 0, nextName = 0;
    const take = (t) => (toks[k].t === t ? toks[k++] : null);
    const err = () => { throw new TypeError("Invalid pattern '" + input + "'"); };
    const flush = () => { if (pending) { parts.push({ type: 'fixed', value: pending, modifier: '' }); pending = ''; } };
    const addPart = (prefix, name, regex, asterisk, suffix, modifier) => {
        let type = 'regexp', value = '';
        if (asterisk) { type = 'full'; value = '.*'; }
        else if (regex !== null) {
            value = regex;
            if (value === segRe) type = 'segment';
            else if (value === '.*') type = 'full';
        } else if (name !== null) { type = 'segment'; value = segRe; }
        else {
            /* no group: fixed text */
            if (!prefix && !suffix && !modifier) return;
            if (!modifier) { pending += prefix + suffix; return; }
            flush();
            parts.push({ type: 'fixed', value: prefix + suffix, modifier });
            return;
        }
        const nm = name !== null ? name : String(nextName++);
        if (parts.some((p) => p.name === nm)) err();
        parts.push({ type, value, name: nm, prefix, suffix, modifier: modifier || '' });
    };
    while (k < toks.length - 1) {
        const ch = take('char');
        let name = take('name');
        let regex = take('regexp');
        if (!name && !regex && toks[k].t === 'asterisk') { /* handled below */ }
        if (name || regex) {
            let prefix = ch ? ch.v : '';
            if (prefix && !prefixChars.includes(prefix)) { pending += prefix; prefix = ''; }
            flush();
            if (name && !regex) regex = take('regexp');
            const mod = take('modifier') || take('asterisk');
            addPart(prefix, name ? name.v : null, regex ? regex.v : null, false, '', mod ? mod.v : '');
            continue;
        }
        if (ch) {
            const ast = toks[k].t === 'asterisk' ? toks[k] : null;
            if (!ast) { pending += ch.v; continue; }
        }
        const ast = take('asterisk');
        if (ast) {
            let prefix = ch ? ch.v : '';
            if (prefix && !prefixChars.includes(prefix)) { pending += prefix; prefix = ''; }
            flush();
            const mod = take('modifier');
            addPart(prefix, null, null, true, '', mod ? mod.v : '');
            continue;
        }
        const esc = take('escaped');
        if (esc) { pending += esc.v; continue; }
        if (take('open')) {
            let prefix = '';
            while (toks[k].t === 'char' || toks[k].t === 'escaped') prefix += toks[k++].v;
            const n2 = take('name');
            const r2 = take('regexp') || null;
            const a2 = !n2 && !r2 ? take('asterisk') : null;
            let suffix = '';
            while (toks[k].t === 'char' || toks[k].t === 'escaped') suffix += toks[k++].v;
            if (!take('close')) err();
            const mod = take('modifier') || take('asterisk');
            flush();
            addPart(prefix, n2 ? n2.v : null, r2 ? r2.v : null, !!a2, suffix, mod ? mod.v : '');
            continue;
        }
        const m = take('modifier');
        if (m) { pending += m.v; continue; }
        err();
    }
    flush();
    return parts;
}

function compile(parts, segRe, ignoreCase) {
    let re = '^';
    const names = [];
    let hasRegExp = false;
    for (const p of parts) {
        if (p.type === 'fixed') {
            re += p.modifier ? '(?:' + escapeRe(p.value) + ')' + p.modifier : escapeRe(p.value);
            continue;
        }
        names.push(p.name);
        if (p.type === 'regexp') hasRegExp = true;
        const val = p.type === 'segment' ? segRe : p.type === 'full' ? '.*' : p.value;
        const pre = escapeRe(p.prefix || ''), suf = escapeRe(p.suffix || '');
        if (!pre && !suf) {
            re += p.modifier === '' || p.modifier === '?' ? '(' + val + ')' + p.modifier : '((?:' + val + ')' + p.modifier + ')';
            continue;
        }
        if (p.modifier === '' || p.modifier === '?') {
            re += '(?:' + pre + '(' + val + ')' + suf + ')' + p.modifier;
            continue;
        }
        re += '(?:' + pre + '((?:' + val + ')(?:' + suf + pre + '(?:' + val + '))*)' + suf + ')' + (p.modifier === '*' ? '?' : '');
    }
    re += '$';
    let rx;
    try { rx = new RegExp(re, ignoreCase ? 'ui' : 'u'); } catch (e) { throw new TypeError('Invalid pattern: ' + e.message); }
    return { rx, names, hasRegExp };
}

/* The pattern string again, normalized (as the standard's "generate a
 * pattern string", simplified). */
function patternString(parts) {
    let s = '';
    for (const p of parts) {
        if (p.type === 'fixed') { s += p.modifier ? '{' + escapePat(p.value) + '}' + p.modifier : escapePat(p.value); continue; }
        const named = !/^\d+$/.test(p.name);
        const custom = p.type === 'regexp';
        const needBrace = p.suffix || (p.prefix && p.prefix !== '/' && p.prefix !== '.');
        if (needBrace) s += '{' + escapePat(p.prefix || '');
        else s += escapePat(p.prefix || '');
        if (p.type === 'full' && !named) s += '*';
        else {
            if (named) s += ':' + p.name;
            if (custom) s += '(' + p.value + ')';
            else if (p.type === 'full') s += '(.*)';
        }
        if (needBrace) s += escapePat(p.suffix || '') + '}';
        s += p.modifier;
    }
    return s;
}

/* ---------------- Inputs ---------------- */

/* Splits a pattern string into components. */
function splitPattern(str, baseURL) {
    const out = {};
    let rest = str;
    const scheme = /^((?:[^:/?#{}()\\]|\\.|\{[^}]*\}[?*+]?|\([^)]*\)[?*+]?)+):\/\//.exec(rest);
    const scan = (s, stops) => {
        let depth = 0;
        for (let i = 0; i < s.length; i++) {
            const c = s[i];
            if (c === '\\') { i++; continue; }
            if (c === '(' || c === '{') depth++;
            else if (c === ')' || c === '}') depth--;
            else if (!depth && stops.includes(c)) {
                /* '?' right after a group or a name is a modifier */
                if (c === '?' && i && /[)}*\w]/.test(s[i - 1]) && !(stops === '?#' && s[i - 1] === '/')) {
                    if (/[)}*]/.test(s[i - 1]) || /:[\w$]+$/.test(s.slice(0, i))) continue;
                }
                return i;
            }
        }
        return -1;
    };
    if (scheme) {
        out.protocol = scheme[1];
        rest = rest.slice(scheme[0].length);
        let a = scan(rest, '/?#');
        const auth = a < 0 ? rest : rest.slice(0, a);
        rest = a < 0 ? '' : rest.slice(a);
        const at = auth.lastIndexOf('@');
        let host = auth;
        if (at >= 0) {
            const cred = auth.slice(0, at);
            host = auth.slice(at + 1);
            const colon = cred.indexOf(':');
            out.username = colon < 0 ? cred : cred.slice(0, colon);
            if (colon >= 0) out.password = cred.slice(colon + 1);
        }
        const portm = /^(.*?[^\\]):(\d*|\*|\([^)]*\))$/.exec(host);
        if (portm && !/^\[/.test(host.slice(portm[1].length))) { out.hostname = portm[1]; out.port = portm[2]; }
        else { out.hostname = host; out.port = ''; }
        if (!rest) out.pathname = SPECIAL[out.protocol + ':'] !== undefined || /^https?$/.test(out.protocol) ? '/' : '';
    } else if (baseURL === undefined) {
        throw new TypeError("A relative pattern '" + str + "' needs a base URL");
    }
    if (rest) {
        const q = scan(rest, '?#');
        if (q < 0) out.pathname = rest;
        else {
            out.pathname = rest.slice(0, q);
            rest = rest.slice(q);
            if (rest[0] === '?') {
                const h = scan(rest.slice(1), '#');
                out.search = h < 0 ? rest.slice(1) : rest.slice(1, h + 1);
                if (h >= 0) out.hash = rest.slice(h + 2);
            } else out.hash = rest.slice(1);
        }
    }
    if (out.pathname !== undefined || out.hostname !== undefined) {
        if (out.search === undefined) out.search = out.hash === undefined ? '*' : '';
        if (out.hash === undefined) out.hash = '*';
    }
    if (baseURL !== undefined) out.baseURL = baseURL;
    return out;
}

/* The components of a base URL. */
function urlParts(u) {
    return {
        protocol: u.protocol.slice(0, -1), username: u.username, password: u.password, hostname: u.hostname,
        port: u.port, pathname: u.pathname, search: u.search.slice(1), hash: u.hash.slice(1),
    };
}

/* An init dictionary filled from its baseURL, as the standard's
 * "process a URLPatternInit" does for patterns. */
function fillInit(init, forPattern) {
    const out = {};
    let base = null;
    if (init.baseURL !== undefined) {
        try { base = urlParts(new URL(String(init.baseURL))); } catch (e) { throw new TypeError('Invalid baseURL'); }
    }
    let inherit = !!base;
    for (const c of COMPONENTS) {
        if (init[c] !== undefined) {
            out[c] = String(init[c]);
            inherit = false;
            continue;
        }
        if (inherit && c !== 'username' && c !== 'password') out[c] = base[c];
        else if (!forPattern) out[c] = '';
    }
    if (base && init.pathname !== undefined && !String(init.pathname).startsWith('/') && init.protocol === undefined &&
        init.hostname === undefined) {
        out.pathname = base.pathname.replace(/[^/]*$/, '') + String(init.pathname);
    }
    if (out.protocol !== undefined) out.protocol = out.protocol.replace(/:$/, '');
    if (out.search !== undefined) out.search = out.search.replace(/^\?/, '');
    if (out.hash !== undefined) out.hash = out.hash.replace(/^#/, '');
    return out;
}

/* ---------------- URLPattern ---------------- */

const P = new WeakMap();
const state = (o) => {
    const s = o !== null && typeof o === 'object' ? P.get(o) : undefined;
    if (!s) throw new TypeError('Illegal invocation');
    return s;
};

class URLPattern {
    constructor(input = {}, baseURL = undefined, options = undefined) {
        if (baseURL !== null && typeof baseURL === 'object' && options === undefined) { options = baseURL; baseURL = undefined; }
        const ignoreCase = !!(options && options.ignoreCase);
        let init;
        if (typeof input === 'string') init = fillInit(splitPattern(input, baseURL), true);
        else {
            if (baseURL !== undefined) throw new TypeError('A base URL goes with a string pattern');
            if (input === null || typeof input !== 'object') throw new TypeError('URLPattern: a string or a dictionary');
            init = fillInit(input, true);
        }
        const comps = {};
        let hasRegExp = false;
        for (const c of COMPONENTS) {
            let pat = init[c] === undefined ? '*' : init[c];
            if (c === 'port' && init.protocol !== undefined && SPECIAL[init.protocol + ':'] !== undefined &&
                String(SPECIAL[init.protocol + ':']) === pat) pat = '';
            if (c === 'protocol') pat = pat.toLowerCase();
            const seg = c === 'pathname' ? '[^/]+?' : c === 'hostname' ? '[^.]+?' : '.+?';
            const pre = c === 'pathname' ? '/' : c === 'hostname' ? '.' : '';
            const parts = parse(pat, seg, pre);
            const comp = compile(parts, seg, ignoreCase && c !== 'protocol');
            comps[c] = { pattern: patternString(parts), ...comp };
            hasRegExp = hasRegExp || comp.hasRegExp;
        }
        P.set(this, { comps, hasRegExp });
    }
    test(input = {}, baseURL = undefined) { return match(state(this), input, baseURL) !== null; }
    exec(input = {}, baseURL = undefined) { return match(state(this), input, baseURL); }
    get hasRegExpGroups() { return state(this).hasRegExp; }
}
for (const c of COMPONENTS)
    Object.defineProperty(URLPattern.prototype, c, {
        get() { return state(this).comps[c].pattern; }, enumerable: true, configurable: true,
    });
Object.defineProperty(URLPattern.prototype, Symbol.toStringTag, { value: 'URLPattern', configurable: true });

function match(s, input, baseURL) {
    let values;
    const inputs = baseURL === undefined ? [input] : [input, baseURL];
    if (typeof input === 'string') {
        let u;
        try { u = baseURL === undefined ? new URL(input) : new URL(input, baseURL); } catch (e) { return null; }
        values = urlParts(u);
    } else {
        if (baseURL !== undefined) throw new TypeError('A base URL goes with a string input');
        if (input === null || typeof input !== 'object') throw new TypeError('URLPattern: a string or a dictionary');
        try { values = fillInit(input, false); } catch (e) { return null; }
    }
    if (values.protocol && SPECIAL[values.protocol + ':'] !== undefined && String(SPECIAL[values.protocol + ':']) === values.port)
        values.port = '';
    const result = { inputs };
    for (const c of COMPONENTS) {
        const comp = s.comps[c];
        const m = comp.rx.exec(values[c] === undefined ? '' : values[c]);
        if (!m) return null;
        const groups = {};
        comp.names.forEach((n, i) => { groups[n] = m[i + 1]; });
        result[c] = { input: values[c] === undefined ? '' : values[c], groups };
    }
    return result;
}

return { URLPattern };
})
