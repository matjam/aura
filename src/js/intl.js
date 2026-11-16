/* Aura's Intl: a compact ECMA-402 subset for English. Every locale a page
 * asks for is formatted as en-US (resolvedOptions().locale says so), the
 * output follows Chrome's for en-US, and there is no locale data beyond
 * the tables below. The build compiles this file to its own bytecode
 * (intl.bc); the prelude reads it with N.intl() the first time a page
 * touches Intl or a toLocale method, so a page that never does pays only
 * for the getter. It is a function of G, the global object, and returns
 * the Intl object and the helpers behind Number, Date and String's locale
 * methods. docs/javascript.md ("Intl") lists what is covered. */
(function (G) {
'use strict';

/* ---------------- common ---------------- */

const slots = new WeakMap();
function inner(o, kind, method) {
    const s = o !== null && typeof o === 'object' ? slots.get(o) : undefined;
    if (!s || s.kind !== kind)
        throw new TypeError('Method Intl.' + kind + '.prototype.' + method + ' called on incompatible receiver');
    return s;
}
const methods = (proto, props) => {
    for (const k of Reflect.ownKeys(props)) {
        const d = Object.getOwnPropertyDescriptor(props, k);
        d.enumerable = false;
        d.configurable = true;
        if ('value' in d) d.writable = true;
        Object.defineProperty(proto, k, d);
    }
};
const tagged = (C, name) =>
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: 'Intl.' + name, configurable: true });
const NBSP = String.fromCharCode(0xa0), THIN = String.fromCharCode(0x2009);
const pad2 = (n) => (n < 10 ? '0' + n : String(n));
const join = (parts) => parts.map((p) => p.value).join('');

function toObj(o) {
    if (o === undefined) return Object.create(null);
    if (o === null) throw new TypeError('Cannot convert undefined or null to object');
    return Object(o);
}
function getStr(o, name, allowed, def) {
    let v = o[name];
    if (v === undefined) return def;
    v = String(v);
    if (allowed && !allowed.includes(v)) throw new RangeError('Value ' + v + ' out of range for Intl options property ' + name);
    return v;
}
function getBool(o, name, def) {
    const v = o[name];
    return v === undefined ? def : Boolean(v);
}
function getNum(o, name, min, max, def) {
    let v = o[name];
    if (v === undefined) return def;
    v = Number(v);
    if (!(v >= min && v <= max)) throw new RangeError(name + ' value is out of range.');
    return Math.floor(v);
}

/* ---------------- locales ---------------- */

const LOCALE = 'en-US';
const TAG_RE = /^(?:[a-z]{2,3}|[a-z]{5,8})(?:-[a-z]{4})?(?:-(?:[a-z]{2}|\d{3}))?(?:-(?:[a-z\d]{5,8}|\d[a-z\d]{3}))*(?:-[\da-wyz](?:-[a-z\d]{2,8})+)*(?:-x(?:-[a-z\d]{1,8})+)?$/i;

function canonTag(t) {
    if (t !== null && typeof t === 'object' && slots.has(t) && slots.get(t).kind === 'Locale') return slots.get(t).tag;
    if (typeof t !== 'string' && (t === null || typeof t !== 'object')) throw new TypeError('Language ID should be string or object.');
    t = String(t);
    if (t.length > 255 || !TAG_RE.test(t)) throw new RangeError('Incorrect locale information provided');
    const p = t.toLowerCase().split('-');
    let i = 1;
    if (p[i] && p[i].length === 4 && !/\d/.test(p[i])) {
        p[i] = p[i][0].toUpperCase() + p[i].slice(1);
        i++;
    }
    if (p[i] && p[i].length === 2) p[i] = p[i].toUpperCase();
    return p.join('-');
}
function localeList(locales) {
    if (locales === undefined) return [];
    if (typeof locales === 'string' || (locales !== null && typeof locales === 'object' && slots.has(locales) &&
                                        slots.get(locales).kind === 'Locale'))
        return [canonTag(locales)];
    if (locales === null) throw new TypeError('Cannot convert undefined or null to object');
    const O = Object(locales), n = Math.min(Number(O.length) || 0, 1000), out = [];
    for (let k = 0; k < n; k++) {
        if (!(k in O)) continue;
        const t = canonTag(O[k]);
        if (!out.includes(t)) out.push(t);
    }
    return out;
}
function supportedLocalesOf(locales, options) {
    const list = localeList(locales);
    if (options !== undefined) getStr(toObj(options), 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    return list.filter((t) => /^en(?:-|$)/.test(t));
}
/* the value of a -u- keyword of the first locale asked for */
function uKey(list, key) {
    if (!list.length) return undefined;
    const m = new RegExp('-u(?:-[a-z\\d]{2,8})*?-' + key + '-([a-z\\d]{3,8})(?:-|$)').exec(list[0]);
    return m ? m[1] : undefined;
}
function startLocale(locales, options) {
    const list = localeList(locales);
    const o = toObj(options);
    getStr(o, 'localeMatcher', ['lookup', 'best fit'], 'best fit');
    return { list, o };
}

/* ---------------- decimal numbers ---------------- */

/* A non-negative decimal: digits D (no leading or trailing zeros; "" is
 * zero) and the place of the point P, so the value is 0.D x 10^P. */
function decOf(x) {
    if (x === 0) return { D: '', P: 0 };
    const s = x.toExponential(), e = s.indexOf('e');
    return { D: (s[0] + s.slice(2, e)).replace(/0+$/, ''), P: +s.slice(e + 1) + 1 };
}
function decOfString(str) {
    const m = /^\s*([+-])?(\d*)(?:\.(\d*))?(?:[eE]([+-]?\d{1,5}))?\s*$/.exec(str);
    if (!m || (!m[2] && !m[3])) return null;
    let D = (m[2] || '') + (m[3] || ''), P = (m[2] || '').length + (m[4] ? +m[4] : 0);
    const lead = /^0*/.exec(D)[0].length;
    D = D.slice(lead).replace(/0+$/, '');
    P -= lead;
    if (!D) P = 0;
    if (P > 400 || P < -400) return null;
    return { neg: m[1] === '-', D, P };
}
function decOfBigInt(b) {
    const s = b.toString();
    return { D: s.replace(/0+$/, '') === '0' ? '' : s.replace(/0+$/, ''), P: s === '0' ? 0 : s.length };
}

/* Keeps the first k digits of d, rounding the rest away by mode. */
function roundDec(d, k, mode, neg) {
    const D = d.D;
    if (k >= D.length) return d;
    const first = k >= 0 ? D.charCodeAt(k) - 48 : 0;
    const rest = k < 0 || D.length > k + 1;
    const half = first > 5 || (first === 5 && rest) ? 1 : first === 5 ? 0 : -1;
    const kept = k > 0 ? D.slice(0, k) : '';
    const odd = kept ? kept.charCodeAt(kept.length - 1) & 1 : 0;
    if (mode === 'ceil') mode = neg ? 'trunc' : 'expand';
    else if (mode === 'floor') mode = neg ? 'expand' : 'trunc';
    else if (mode === 'halfCeil') mode = neg ? 'halfTrunc' : 'halfExpand';
    else if (mode === 'halfFloor') mode = neg ? 'halfExpand' : 'halfTrunc';
    const up = mode === 'expand' ? true : mode === 'trunc' ? false : mode === 'halfTrunc' ? half > 0 :
        mode === 'halfEven' ? half > 0 || (half === 0 && odd === 1) : half >= 0;
    if (!up) {
        const r = kept.replace(/0+$/, '');
        return r ? { D: r, P: d.P } : { D: '', P: 0 };
    }
    if (!kept) return { D: '1', P: d.P - k + 1 };
    const a = kept.split('');
    let i = a.length - 1;
    while (i >= 0 && a[i] === '9') a[i--] = '0';
    if (i < 0) return { D: '1', P: d.P + 1 };
    a[i] = String.fromCharCode(a[i].charCodeAt(0) + 1);
    return { D: a.join('').replace(/0+$/, ''), P: d.P };
}

/* digit options (SetNumberFormatDigitOptions) */
function digitOptions(s, o, mnfdDef, mxfdDef, notation) {
    s.mnid = getNum(o, 'minimumIntegerDigits', 1, 21, 1);
    let mnfd = o.minimumFractionDigits, mxfd = o.maximumFractionDigits;
    let mnsd = o.minimumSignificantDigits, mxsd = o.maximumSignificantDigits;
    s.roundingIncrement = getNum(o, 'roundingIncrement', 1, 5000, 1);
    s.roundingMode = getStr(o, 'roundingMode', ['ceil', 'floor', 'expand', 'trunc', 'halfCeil', 'halfFloor', 'halfExpand',
                                                 'halfTrunc', 'halfEven'], 'halfExpand');
    s.roundingPriority = getStr(o, 'roundingPriority', ['auto', 'morePrecision', 'lessPrecision'], 'auto');
    s.trailingZeroDisplay = getStr(o, 'trailingZeroDisplay', ['auto', 'stripIfInteger'], 'auto');
    const hasSd = mnsd !== undefined || mxsd !== undefined, hasFd = mnfd !== undefined || mxfd !== undefined;
    let needSd = true, needFd = true;
    if (s.roundingPriority === 'auto') {
        needSd = hasSd;
        if (needSd || (!hasFd && notation === 'compact')) needFd = false;
    }
    if (needSd) {
        if (hasSd) {
            mnsd = getNum(o, 'minimumSignificantDigits', 1, 21, 1);
            mxsd = getNum(o, 'maximumSignificantDigits', mnsd, 21, 21);
        } else {
            mnsd = 1;
            mxsd = 21;
        }
        s.mnsd = mnsd;
        s.mxsd = mxsd;
    }
    if (needFd) {
        if (hasFd) {
            mnfd = getNum(o, 'minimumFractionDigits', 0, 100, undefined);
            mxfd = getNum(o, 'maximumFractionDigits', 0, 100, undefined);
            if (mnfd === undefined) mnfd = Math.min(mnfdDef, mxfd);
            else if (mxfd === undefined) mxfd = Math.max(mxfdDef, mnfd);
            else if (mnfd > mxfd) throw new RangeError('maximumFractionDigits value is out of range.');
        } else {
            mnfd = mnfdDef;
            mxfd = mxfdDef;
        }
        s.mnfd = mnfd;
        s.mxfd = mxfd;
    }
    if (!needSd && !needFd) {
        s.mnfd = 0;
        s.mxfd = 0;
        s.mnsd = 1;
        s.mxsd = 2;
        s.roundingType = 'morePrecision';
        s.roundingPriority = 'morePrecision';
    } else if (needSd && needFd) {
        s.roundingType = s.roundingPriority;
    } else {
        s.roundingType = hasSd ? 'significantDigits' : 'fractionDigits';
    }
}

/* Rounds d by the digit options s: { int, frac } as digit strings. */
function roundDigits(s, d, neg) {
    const fixed = () => ({ r: roundDec(d, d.P + s.mxfd, s.roundingMode, neg), mag: -s.mxfd, mn: s.mnfd, sig: false });
    const prec = () => ({ r: roundDec(d, s.mxsd, s.roundingMode, neg), mag: d.D ? d.P - s.mxsd : 1 - s.mxsd, mn: 0, sig: true });
    let res;
    if (s.roundingType === 'fractionDigits') res = fixed();
    else if (s.roundingType === 'significantDigits') res = prec();
    else {
        const f = fixed(), p = prec();
        const fixedMore = f.mag <= p.mag;
        res = s.roundingType === 'morePrecision' ? (fixedMore ? f : p) : (fixedMore ? p : f);
    }
    const r = res.r;
    let int, frac;
    if (!r.D) {
        int = '0';
        frac = '';
    } else if (r.P > 0) {
        int = r.D.slice(0, r.P).padEnd(r.P, '0');
        frac = r.D.slice(r.P);
    } else {
        int = '0';
        frac = '0'.repeat(-r.P) + r.D;
    }
    let mn = res.sig ? (r.D ? s.mnsd - r.P : s.mnsd - 1) : res.mn;
    if (s.trailingZeroDisplay === 'stripIfInteger' && !frac) mn = 0;
    if (mn > frac.length) frac = frac.padEnd(mn, '0');
    if (int.length < s.mnid) int = int.padStart(s.mnid, '0');
    return { int, frac };
}

/* ---------------- NumberFormat ---------------- */

const CURRENCY = {
    USD: ['$', 'US dollar', 'US dollars'], EUR: ['€', 'euro', 'euros'], GBP: ['£', 'British pound', 'British pounds'],
    JPY: ['¥', 'Japanese yen', 'Japanese yen'], CAD: ['CA$', 'Canadian dollar', 'Canadian dollars'],
    AUD: ['A$', 'Australian dollar', 'Australian dollars'], CNY: ['CN¥', 'Chinese yuan', 'Chinese yuan'],
    INR: ['₹', 'Indian rupee', 'Indian rupees'], MXN: ['MX$', 'Mexican peso', 'Mexican pesos'],
    BRL: ['R$', 'Brazilian real', 'Brazilian reals'], KRW: ['₩', 'South Korean won', 'South Korean won'],
    HKD: ['HK$', 'Hong Kong dollar', 'Hong Kong dollars'], NZD: ['NZ$', 'New Zealand dollar', 'New Zealand dollars'],
    TWD: ['NT$', 'New Taiwan dollar', 'New Taiwan dollars'], ILS: ['₪', 'Israeli new shekel', 'Israeli new shekels'],
    VND: ['₫', 'Vietnamese dong', 'Vietnamese dong'], CHF: [null, 'Swiss franc', 'Swiss francs'],
};
const CUR0 = ' BIF CLP DJF GNF ISK JPY KMF KRW PYG RWF UGX UYI VND VUV XAF XOF XPF ';
const CUR3 = ' BHD IQD JOD KWD LYD OMR TND ';
const currencyDigits = (c) => (CUR0.includes(' ' + c + ' ') ? 0 : CUR3.includes(' ' + c + ' ') ? 3 : 2);

/* short (singular/plural) | narrow | long (singular/plural) */
const UNITS = {
    acre: 'ac|ac|acre', bit: 'bit|bit|bit', byte: 'byte|B|byte', celsius: '°C|°C|degree Celsius/degrees Celsius',
    centimeter: 'cm|cm|centimeter', day: 'day/days|d|day', degree: 'deg|°|degree',
    fahrenheit: '°F|°|degree Fahrenheit/degrees Fahrenheit', 'fluid-ounce': 'fl oz|fl oz|fluid ounce',
    foot: 'ft|′|foot/feet', gallon: 'gal|gal|gallon', gigabit: 'Gb|Gb|gigabit', gigabyte: 'GB|GB|gigabyte',
    gram: 'g|g|gram', hectare: 'ha|ha|hectare', hour: 'hr|h|hour', inch: 'in|″|inch/inches', kilobit: 'kb|kb|kilobit',
    kilobyte: 'kB|kB|kilobyte', kilogram: 'kg|kg|kilogram', kilometer: 'km|km|kilometer', liter: 'L|L|liter',
    megabit: 'Mb|Mb|megabit', megabyte: 'MB|MB|megabyte', meter: 'm|m|meter', microsecond: 'μs|μs|microsecond',
    mile: 'mi|mi|mile', 'mile-scandinavian': 'smi|smi|mile-scandinavian', milliliter: 'mL|mL|milliliter',
    millimeter: 'mm|mm|millimeter', millisecond: 'ms|ms|millisecond', minute: 'min|m|minute', month: 'mth/mths|m|month',
    nanosecond: 'ns|ns|nanosecond', ounce: 'oz|oz|ounce', percent: '%|%|percent/percent', petabyte: 'PB|PB|petabyte',
    pound: 'lb|lb|pound', second: 'sec|s|second', stone: 'st|st|stone', terabit: 'Tb|Tb|terabit',
    terabyte: 'TB|TB|terabyte', week: 'wk/wks|w|week', yard: 'yd|yd|yard', year: 'yr/yrs|y|year',
};
const SPECIAL_PER = { 'kilometer-per-hour': 'km/h', 'mile-per-hour': 'mph' };
function unitInfo(u) {
    const e = UNITS[u].split('|'), sh = e[0].split('/'), lo = e[2].split('/');
    return { s1: sh[0], s2: sh[1] || sh[0], n: e[1], l1: lo[0], l2: lo[1] || lo[0] + 's' };
}
function validUnit(u) {
    if (Object.prototype.hasOwnProperty.call(UNITS, u)) return true;
    const i = u.indexOf('-per-');
    return i > 0 && Object.prototype.hasOwnProperty.call(UNITS, u.slice(0, i)) &&
        Object.prototype.hasOwnProperty.call(UNITS, u.slice(i + 5));
}
/* the unit's text for a formatted number and its spacing */
function unitText(u, disp, one) {
    const i = u.indexOf('-per-');
    if (i > 0) {
        const a = unitInfo(u.slice(0, i)), b = unitInfo(u.slice(i + 5));
        if (disp === 'long') return { t: (one ? a.l1 : a.l2) + ' per ' + b.l1, sp: ' ' };
        if (disp === 'short' && SPECIAL_PER[u]) return { t: SPECIAL_PER[u], sp: ' ' };
        return { t: (disp === 'narrow' ? a.n : a.s1) + '/' + (disp === 'narrow' ? b.n : b.s1), sp: disp === 'narrow' ? '' : ' ' };
    }
    const n = unitInfo(u);
    if (disp === 'long') return { t: one ? n.l1 : n.l2, sp: ' ' };
    if (disp === 'narrow') return { t: n.n, sp: '' };
    const t = one ? n.s1 : n.s2;
    return { t, sp: t[0] === '%' || t[0] === '°' ? '' : ' ' };
}

function initNumberFormat(nf, locales, options) {
    const { o } = startLocale(locales, options);
    const s = { kind: 'NumberFormat', locale: LOCALE, numberingSystem: 'latn', bound: undefined };
    getStr(o, 'numberingSystem', null, undefined);
    s.style = getStr(o, 'style', ['decimal', 'percent', 'currency', 'unit'], 'decimal');
    let cur = getStr(o, 'currency', null, undefined);
    if (cur !== undefined && !/^[A-Za-z]{3}$/.test(cur)) throw new RangeError('Invalid currency code : ' + cur);
    const cd = getStr(o, 'currencyDisplay', ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol');
    const cs = getStr(o, 'currencySign', ['standard', 'accounting'], 'standard');
    const unit = getStr(o, 'unit', null, undefined);
    if (unit !== undefined && !validUnit(unit)) throw new RangeError('Invalid unit argument for Intl.NumberFormat() \'' + unit + '\'');
    const ud = getStr(o, 'unitDisplay', ['short', 'narrow', 'long'], 'short');
    if (s.style === 'currency') {
        if (cur === undefined) throw new TypeError('Currency code is required with currency style.');
        s.currency = cur.toUpperCase();
        s.currencyDisplay = cd;
        s.currencySign = cs;
    }
    if (s.style === 'unit') {
        if (unit === undefined) throw new TypeError('Unit is required with unit style.');
        s.unit = unit;
        s.unitDisplay = ud;
    }
    s.notation = getStr(o, 'notation', ['standard', 'scientific', 'engineering', 'compact'], 'standard');
    const cdig = s.style === 'currency' && s.notation === 'standard' ? currencyDigits(s.currency) : 0;
    digitOptions(s, o, cdig, s.style === 'currency' && s.notation === 'standard' ? cdig : s.style === 'percent' ? 0 : 3,
                 s.notation);
    s.compactDisplay = getStr(o, 'compactDisplay', ['short', 'long'], 'short');
    const ug = o.useGrouping;
    const ugDef = s.notation === 'compact' ? 'min2' : 'auto';
    if (ug === undefined) s.useGrouping = ugDef;
    else if (ug === true) s.useGrouping = 'always';
    else if (ug === false || ug === null || ug === 0 || ug === '') s.useGrouping = false;
    else {
        const v = String(ug);
        if (v === 'true' || v === 'false') s.useGrouping = ugDef;
        else if (v === 'min2' || v === 'auto' || v === 'always') s.useGrouping = v;
        else throw new RangeError('Value ' + v + ' out of range for Intl.NumberFormat options property useGrouping');
    }
    s.signDisplay = getStr(o, 'signDisplay', ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto');
    slots.set(nf, s);
    return nf;
}

function groupInt(int, g) {
    if (!g || int.length < 4 || (g === 'min2' && int.length < 5)) return [int];
    const out = [];
    let i = int.length % 3 || 3;
    out.push(int.slice(0, i));
    for (; i < int.length; i += 3) out.push(int.slice(i, i + 3));
    return out;
}

const COMPACT_S = ['', 'K', 'M', 'B', 'T'], COMPACT_L = ['', 'thousand', 'million', 'billion', 'trillion'];

/* The parts of x formatted by s; unitTag adds a unit property to the
 * number's parts (RelativeTimeFormat). */
function numParts(s, x, unitTag) {
    let neg = false, d = null, special = null;
    if (typeof x === 'bigint') {
        neg = x < 0n;
        d = decOfBigInt(neg ? -x : x);
    } else {
        if (typeof x === 'string') {
            const r = decOfString(x);
            if (r) {
                neg = r.neg;
                d = r;
            } else {
                x = Number(x);
            }
        } else {
            x = Number(x);
        }
        if (!d) {
            if (x !== x) special = { type: 'nan', value: 'NaN' };
            else {
                neg = x < 0 || Object.is(x, -0);
                if (x === Infinity || x === -Infinity) special = { type: 'infinity', value: '∞' };
                else d = decOf(Math.abs(x));
            }
        }
    }
    const num = [];
    let zero = false, oneish = false;
    if (special) {
        num.push(special);
    } else {
        if (s.style === 'percent' && d.D) d = { D: d.D, P: d.P + 2 };
        let ex = 0, r;
        if (s.notation === 'compact' && d.D) {
            const mag = d.P - 1;
            ex = mag < 3 ? 0 : Math.min(Math.floor(mag / 3) * 3, 12);
            r = roundDigits(s, { D: d.D, P: d.P - ex }, neg);
            if (ex < 12 && r.int.length > 3) {
                ex += 3;
                r = roundDigits(s, { D: d.D, P: d.P - ex }, neg);
            }
        } else if ((s.notation === 'scientific' || s.notation === 'engineering') && d.D) {
            const step = s.notation === 'engineering' ? 3 : 1;
            ex = Math.floor((d.P - 1) / step) * step;
            r = roundDigits(s, { D: d.D, P: d.P - ex }, neg);
            if (r.int.length > step) {
                ex += step;
                r = roundDigits(s, { D: d.D, P: d.P - ex }, neg);
            }
        } else {
            r = roundDigits(s, d, neg);
        }
        zero = /^0*$/.test(r.int + r.frac);
        oneish = r.int === '1' && !r.frac && !ex;
        const groups = groupInt(r.int, s.useGrouping);
        groups.forEach((g, i) => {
            if (i) num.push({ type: 'group', value: ',' });
            num.push({ type: 'integer', value: g });
        });
        if (r.frac) num.push({ type: 'decimal', value: '.' }, { type: 'fraction', value: r.frac });
        if (s.notation === 'compact' && ex) {
            if (s.compactDisplay === 'long') num.push({ type: 'literal', value: ' ' }, { type: 'compact', value: COMPACT_L[ex / 3] });
            else num.push({ type: 'compact', value: COMPACT_S[ex / 3] });
        } else if (s.notation === 'scientific' || s.notation === 'engineering') {
            num.push({ type: 'exponentSeparator', value: 'E' });
            if (ex < 0) num.push({ type: 'exponentMinusSign', value: '-' });
            num.push({ type: 'exponentInteger', value: String(Math.abs(ex)) });
        }
    }
    if (unitTag) for (const p of num) p.unit = unitTag;
    let sign = null;
    switch (special && special.type === 'nan' ? 'nan' : s.signDisplay) {
    case 'auto': if (neg) sign = '-'; break;
    case 'always': sign = neg ? '-' : '+'; break;
    case 'exceptZero': if (!zero) sign = neg ? '-' : '+'; break;
    case 'negative': if (neg && !zero) sign = '-'; break;
    }
    const out = [];
    const signPart = () => {
        if (sign) out.push({ type: sign === '-' ? 'minusSign' : 'plusSign', value: sign });
    };
    if (s.style === 'currency') {
        const c = CURRENCY[s.currency];
        const accounting = s.currencySign === 'accounting' && sign === '-';
        if (s.currencyDisplay === 'name') {
            signPart();
            out.push(...num, { type: 'literal', value: ' ' }, { type: 'currency', value: c ? (oneish ? c[1] : c[2]) : s.currency });
            return out;
        }
        let sym = s.currency, code = true;
        if (c && c[0] && s.currencyDisplay !== 'code') {
            sym = s.currencyDisplay === 'narrowSymbol' ? c[0].replace(/^[A-Z]+/, '') : c[0];
            code = false;
        }
        if (accounting) out.push({ type: 'literal', value: '(' });
        else signPart();
        out.push({ type: 'currency', value: sym });
        if (code) out.push({ type: 'literal', value: NBSP });
        out.push(...num);
        if (accounting) out.push({ type: 'literal', value: ')' });
        return out;
    }
    signPart();
    out.push(...num);
    if (s.style === 'percent') out.push({ type: 'percentSign', value: '%' });
    else if (s.style === 'unit') {
        const u = unitText(s.unit, s.unitDisplay, oneish);
        if (u.sp) out.push({ type: 'literal', value: u.sp });
        out.push({ type: 'unit', value: u.t });
    }
    return out;
}
const numFormat = (s, x) => join(numParts(s, x));

function NumberFormat(locales, options) {
    if (!new.target) return new NumberFormat(locales, options);
    initNumberFormat(this, locales, options);
}
methods(NumberFormat.prototype, {
    get format() {
        const s = inner(this, 'NumberFormat', 'format');
        if (!s.bound) {
            const f = (x) => numFormat(s, x);
            Object.defineProperty(f, 'name', { value: '' });
            s.bound = f;
        }
        return s.bound;
    },
    formatToParts(x) {
        return numParts(inner(this, 'NumberFormat', 'formatToParts'), x);
    },
    formatRange(a, b) {
        const s = inner(this, 'NumberFormat', 'formatRange');
        if (a === undefined || b === undefined) throw new TypeError('start or end is undefined');
        const x = numFormat(s, a), y = numFormat(s, b);
        return x === y ? '~' + x : x + '–' + y;
    },
    resolvedOptions() {
        const s = inner(this, 'NumberFormat', 'resolvedOptions');
        const r = { locale: s.locale, numberingSystem: s.numberingSystem, style: s.style };
        if (s.style === 'currency') {
            r.currency = s.currency;
            r.currencyDisplay = s.currencyDisplay;
            r.currencySign = s.currencySign;
        }
        if (s.style === 'unit') {
            r.unit = s.unit;
            r.unitDisplay = s.unitDisplay;
        }
        r.minimumIntegerDigits = s.mnid;
        if (s.mnfd !== undefined) {
            r.minimumFractionDigits = s.mnfd;
            r.maximumFractionDigits = s.mxfd;
        }
        if (s.mnsd !== undefined) {
            r.minimumSignificantDigits = s.mnsd;
            r.maximumSignificantDigits = s.mxsd;
        }
        r.useGrouping = s.useGrouping;
        r.notation = s.notation;
        if (s.notation === 'compact') r.compactDisplay = s.compactDisplay;
        r.signDisplay = s.signDisplay;
        r.roundingIncrement = s.roundingIncrement;
        r.roundingMode = s.roundingMode;
        r.roundingPriority = s.roundingPriority;
        r.trailingZeroDisplay = s.trailingZeroDisplay;
        return r;
    },
});
NumberFormat.supportedLocalesOf = supportedLocalesOf;
tagged(NumberFormat, 'NumberFormat');

/* ---------------- PluralRules ---------------- */

function pluralOf(s, x) {
    const n = Number(x);
    if (!isFinite(n)) return 'other';
    const r = roundDigits(s, decOf(Math.abs(n)), n < 0);
    if (s.type === 'ordinal') {
        if (r.frac) return 'other';
        const i = r.int, t = +i.slice(-2), u = t % 10;
        return u === 1 && t !== 11 ? 'one' : u === 2 && t !== 12 ? 'two' : u === 3 && t !== 13 ? 'few' : 'other';
    }
    return r.int === '1' && !r.frac ? 'one' : 'other';
}
class PluralRules {
    constructor(locales, options) {
        const { o } = startLocale(locales, options);
        const s = { kind: 'PluralRules', locale: LOCALE };
        s.type = getStr(o, 'type', ['cardinal', 'ordinal'], 'cardinal');
        digitOptions(s, o, 0, 3, 'standard');
        slots.set(this, s);
    }
    select(x) {
        return pluralOf(inner(this, 'PluralRules', 'select'), x);
    }
    selectRange(a, b) {
        const s = inner(this, 'PluralRules', 'selectRange');
        if (a === undefined || b === undefined) throw new TypeError('start or end is undefined');
        return pluralOf(s, b);
    }
    resolvedOptions() {
        const s = inner(this, 'PluralRules', 'resolvedOptions');
        const r = { locale: s.locale, type: s.type, minimumIntegerDigits: s.mnid };
        if (s.mnfd !== undefined) {
            r.minimumFractionDigits = s.mnfd;
            r.maximumFractionDigits = s.mxfd;
        }
        if (s.mnsd !== undefined) {
            r.minimumSignificantDigits = s.mnsd;
            r.maximumSignificantDigits = s.mxsd;
        }
        r.pluralCategories = s.type === 'ordinal' ? ['few', 'one', 'two', 'other'] : ['one', 'other'];
        r.roundingIncrement = s.roundingIncrement;
        r.roundingMode = s.roundingMode;
        r.roundingPriority = s.roundingPriority;
        r.trailingZeroDisplay = s.trailingZeroDisplay;
        return r;
    }
    static supportedLocalesOf(l, o) {
        return supportedLocalesOf(l, o);
    }
}
tagged(PluralRules, 'PluralRules');

/* ---------------- time zones ---------------- */

/* name: standard offset in minutes east of UTC, daylight rule, and the
 * US abbreviation letters. Rules: us (second Sunday of March to first
 * Sunday of November, 02:00 local), eu (last Sunday of March to last
 * Sunday of October, 01:00 UTC), au (first Sunday of October to first
 * Sunday of April), nz (last Sunday of September to first Sunday of April). */
const ZONES = {
    'America/New_York': [-300, 'us', 'E'], 'America/Chicago': [-360, 'us', 'C'], 'America/Denver': [-420, 'us', 'M'],
    'America/Phoenix': [-420, '', 'M'], 'America/Los_Angeles': [-480, 'us', 'P'], 'America/Anchorage': [-540, 'us', 'AK'],
    'Pacific/Honolulu': [-600, '', 'H'], 'America/Detroit': [-300, 'us', 'E'], 'America/Toronto': [-300, 'us', 'E'],
    'America/Vancouver': [-480, 'us', 'P'], 'America/Edmonton': [-420, 'us', 'M'], 'America/Winnipeg': [-360, 'us', 'C'],
    'America/Halifax': [-240, 'us', 'A'], 'America/Mexico_City': [-360, '', 'C'], 'America/Bogota': [-300, ''],
    'America/Lima': [-300, ''], 'America/Sao_Paulo': [-180, ''], 'America/Argentina/Buenos_Aires': [-180, ''],
    'Europe/London': [0, 'eu'], 'Europe/Dublin': [0, 'eu'], 'Europe/Lisbon': [0, 'eu'], 'Europe/Paris': [60, 'eu'],
    'Europe/Berlin': [60, 'eu'], 'Europe/Madrid': [60, 'eu'], 'Europe/Rome': [60, 'eu'], 'Europe/Amsterdam': [60, 'eu'],
    'Europe/Brussels': [60, 'eu'], 'Europe/Vienna': [60, 'eu'], 'Europe/Zurich': [60, 'eu'], 'Europe/Stockholm': [60, 'eu'],
    'Europe/Oslo': [60, 'eu'], 'Europe/Copenhagen': [60, 'eu'], 'Europe/Warsaw': [60, 'eu'], 'Europe/Prague': [60, 'eu'],
    'Europe/Budapest': [60, 'eu'], 'Europe/Athens': [120, 'eu'], 'Europe/Helsinki': [120, 'eu'], 'Europe/Kyiv': [120, 'eu'],
    'Europe/Kiev': [120, 'eu'], 'Europe/Bucharest': [120, 'eu'], 'Europe/Istanbul': [180, ''], 'Europe/Moscow': [180, ''],
    'Africa/Lagos': [60, ''], 'Africa/Johannesburg': [120, ''], 'Africa/Nairobi': [180, ''], 'Asia/Dubai': [240, ''],
    'Asia/Tehran': [210, ''], 'Asia/Karachi': [300, ''], 'Asia/Kolkata': [330, ''], 'Asia/Calcutta': [330, ''],
    'Asia/Dhaka': [360, ''], 'Asia/Bangkok': [420, ''], 'Asia/Jakarta': [420, ''], 'Asia/Ho_Chi_Minh': [420, ''],
    'Asia/Shanghai': [480, ''], 'Asia/Hong_Kong': [480, ''], 'Asia/Singapore': [480, ''], 'Asia/Taipei': [480, ''],
    'Asia/Manila': [480, ''], 'Australia/Perth': [480, ''], 'Asia/Tokyo': [540, ''], 'Asia/Seoul': [540, ''],
    'Australia/Adelaide': [570, 'au'], 'Australia/Brisbane': [600, ''], 'Australia/Sydney': [600, 'au'],
    'Australia/Melbourne': [600, 'au'], 'Pacific/Auckland': [720, 'nz'],
};
const US_NAMES = { E: 'Eastern', C: 'Central', M: 'Mountain', P: 'Pacific', AK: 'Alaska', H: 'Hawaii-Aleutian', A: 'Atlantic' };
let zoneKeys = null;

const offStr = (off, long) => {
    const a = Math.abs(off), h = Math.floor(a / 60), m = a % 60;
    if (!off) return 'GMT';
    return 'GMT' + (off < 0 ? '-' : '+') + (long ? pad2(h) + ':' + pad2(m) : h + (m ? ':' + pad2(m) : ''));
};
function localZoneId() {
    const off = -new Date().getTimezoneOffset();
    if (!off) return 'UTC';
    if (off % 60 === 0) return 'Etc/GMT' + (off > 0 ? '-' : '+') + Math.abs(off / 60);
    const a = Math.abs(off);
    return (off < 0 ? '-' : '+') + pad2(Math.floor(a / 60)) + ':' + pad2(a % 60);
}
function zoneOf(tz) {
    if (tz === undefined) return { id: localZoneId(), local: true };
    const t = String(tz), u = t.toUpperCase();
    if (/^(?:ETC\/)?(?:UTC|GMT|UCT|UNIVERSAL|ZULU|GREENWICH)(?:[+-]0)?$/.test(u)) return { id: 'UTC', off: 0 };
    let m = /^ETC\/GMT([+-])(\d{1,2})$/.exec(u);
    if (m && +m[2] <= (m[1] === '-' ? 14 : 12)) {
        const h = +m[2];
        return { id: 'Etc/GMT' + m[1] + h, off: m[1] === '-' ? h * 60 : -h * 60 };
    }
    m = /^([+-])(\d{2})(?::?(\d{2}))?$/.exec(t);
    if (m && +m[2] <= 23 && (!m[3] || +m[3] <= 59)) {
        const off = (+m[2] * 60 + (m[3] ? +m[3] : 0)) * (m[1] === '-' ? -1 : 1);
        return { id: m[1] + m[2] + ':' + (m[3] || '00'), off };
    }
    if (!zoneKeys) {
        zoneKeys = Object.create(null);
        for (const k of Object.keys(ZONES)) zoneKeys[k.toUpperCase()] = k;
    }
    const id = zoneKeys[u];
    if (id === undefined) {
        if (u === localZoneId().toUpperCase()) return { id: localZoneId(), local: true };
        throw new RangeError('Invalid time zone specified: ' + t);
    }
    const z = ZONES[id];
    return { id, off: z[0], rule: z[1], us: z[2] };
}
function nthSunday(y, mon, n) {
    if (n > 0) {
        const d1 = new Date(Date.UTC(y, mon, 1)).getUTCDay();
        return Date.UTC(y, mon, 1 + ((7 - d1) % 7) + 7 * (n - 1));
    }
    const last = new Date(Date.UTC(y, mon + 1, 0));
    return Date.UTC(y, mon, last.getUTCDate() - last.getUTCDay());
}
function zoneOffset(z, t) {
    if (!z.rule) return z.off;
    const H = 3600000, so = z.off * 60000, y = new Date(t + so).getUTCFullYear();
    let a, b;
    switch (z.rule) {
    case 'us':
        a = nthSunday(y, 2, 2) + 2 * H - so;
        b = nthSunday(y, 10, 1) + H - so;
        return t >= a && t < b ? z.off + 60 : z.off;
    case 'eu':
        a = nthSunday(y, 2, -1) + H;
        b = nthSunday(y, 9, -1) + H;
        return t >= a && t < b ? z.off + 60 : z.off;
    case 'au':
        b = nthSunday(y, 3, 1) + 2 * H - so;
        a = nthSunday(y, 9, 1) + 2 * H - so;
        return t < b || t >= a ? z.off + 60 : z.off;
    default: /* nz */
        b = nthSunday(y, 3, 1) + 2 * H - so;
        a = nthSunday(y, 8, -1) + 2 * H - so;
        return t < b || t >= a ? z.off + 60 : z.off;
    }
}
function fieldsAt(t, z) {
    let d, off;
    if (z.local) {
        d = new Date(t);
        return { y: d.getFullYear(), M: d.getMonth(), d: d.getDate(), w: d.getDay(), h: d.getHours(), m: d.getMinutes(),
                 s: d.getSeconds(), ms: d.getMilliseconds(), off: -d.getTimezoneOffset() };
    }
    off = zoneOffset(z, t);
    d = new Date(t + off * 60000);
    if (d.getTime() !== d.getTime()) d = new Date(t);
    return { y: d.getUTCFullYear(), M: d.getUTCMonth(), d: d.getUTCDate(), w: d.getUTCDay(), h: d.getUTCHours(),
             m: d.getUTCMinutes(), s: d.getUTCSeconds(), ms: d.getUTCMilliseconds(), off };
}
function zoneName(z, f, style) {
    const dst = !z.local && z.rule && f.off !== z.off;
    if (z.id === 'UTC' && (style === 'short' || style === 'long'))
        return style === 'long' ? 'Coordinated Universal Time' : 'UTC';
    if (z.us) {
        if (style === 'short') return z.us + (dst ? 'D' : 'S') + 'T';
        if (style === 'long') return US_NAMES[z.us] + (dst ? ' Daylight' : ' Standard') + ' Time';
        if (style === 'shortGeneric') return z.us + 'T';
        if (style === 'longGeneric') return US_NAMES[z.us] + ' Time';
    }
    if (z.id === 'Europe/London' && style === 'long') return dst ? 'British Summer Time' : 'Greenwich Mean Time';
    if (z.id === 'UTC' && /Generic$/.test(style)) return 'GMT';
    return offStr(f.off, style === 'long' || style === 'longOffset');
}

/* ---------------- DateTimeFormat ---------------- */

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October',
                'November', 'December'];
const DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const nameOf = (list, i, w) => (w === 'long' ? list[i] : w === 'short' ? list[i].slice(0, 3) : list[i][0]);
const TEXT3 = ['narrow', 'short', 'long'], NUM2 = ['2-digit', 'numeric'];
const DT_FIELDS = [['weekday', TEXT3], ['era', TEXT3], ['year', NUM2], ['month', ['2-digit', 'numeric', 'narrow', 'short', 'long']],
                   ['day', NUM2], ['dayPeriod', TEXT3], ['hour', NUM2], ['minute', NUM2], ['second', NUM2],
                   ['fractionalSecondDigits', null],
                   ['timeZoneName', ['short', 'long', 'shortOffset', 'longOffset', 'shortGeneric', 'longGeneric']]];
const STYLES = ['full', 'long', 'medium', 'short'];
const DATE_STYLE = {
    full: { weekday: 'long', month: 'long', day: 'numeric', year: 'numeric' },
    long: { month: 'long', day: 'numeric', year: 'numeric' },
    medium: { month: 'short', day: 'numeric', year: 'numeric' },
    short: { month: 'numeric', day: 'numeric', year: '2-digit' },
};
const TIME_STYLE = {
    full: { hour: 'numeric', minute: '2-digit', second: '2-digit', timeZoneName: 'long' },
    long: { hour: 'numeric', minute: '2-digit', second: '2-digit', timeZoneName: 'short' },
    medium: { hour: 'numeric', minute: '2-digit', second: '2-digit' },
    short: { hour: 'numeric', minute: '2-digit' },
};

/* required: 'date', 'time' or 'any'; defaults: 'date', 'time' or 'all' */
function initDateTimeFormat(dtf, locales, options, required, defaults) {
    const { list, o } = startLocale(locales, options);
    const s = { kind: 'DateTimeFormat', locale: LOCALE, calendar: 'gregory', numberingSystem: 'latn', bound: undefined };
    getStr(o, 'calendar', null, undefined);
    getStr(o, 'numberingSystem', null, undefined);
    const hour12 = o.hour12 === undefined ? undefined : Boolean(o.hour12);
    let hc = getStr(o, 'hourCycle', ['h11', 'h12', 'h23', 'h24'], undefined);
    if (hour12 !== undefined) hc = hour12 ? 'h12' : 'h23';
    else if (hc === undefined) hc = uKey(list, 'hc');
    if (!/^h(?:11|12|23|24)$/.test(hc || '')) hc = 'h12';
    else if (hour12 === undefined && o.hourCycle === undefined) s.locale = LOCALE + '-u-hc-' + hc;
    s.zone = zoneOf(o.timeZone);
    const f = {};
    let any = false;
    for (const [name, allowed] of DT_FIELDS) {
        const v = allowed ? getStr(o, name, allowed, undefined) : getNum(o, name, 1, 3, undefined);
        if (v !== undefined) {
            f[name] = v;
            any = true;
        }
    }
    getStr(o, 'formatMatcher', ['basic', 'best fit'], 'best fit');
    const ds = getStr(o, 'dateStyle', STYLES, undefined), ts = getStr(o, 'timeStyle', STYLES, undefined);
    if (ds || ts) {
        if (any) throw new TypeError('Can\'t set option ' + Object.keys(f)[0] + ' when ' + (ds ? 'dateStyle' : 'timeStyle') + ' is used');
        if (required === 'date' && !ds) throw new TypeError('Invalid option : timeStyle');
        if (required === 'time' && !ts) throw new TypeError('Invalid option : dateStyle');
        s.dateStyle = ds;
        s.timeStyle = ts;
        if (ds) Object.assign(f, DATE_STYLE[ds]);
        if (ts) Object.assign(f, TIME_STYLE[ts]);
    } else {
        let need = true;
        if (required !== 'time' && (f.weekday || f.year || f.month || f.day)) need = false;
        if (required !== 'date' && (f.dayPeriod || f.hour || f.minute || f.second || f.fractionalSecondDigits)) need = false;
        if (need && (defaults === 'date' || defaults === 'all')) f.year = f.month = f.day = 'numeric';
        if (need && (defaults === 'time' || defaults === 'all')) f.hour = f.minute = f.second = 'numeric';
    }
    if (f.hour) s.hourCycle = hc;
    s.f = f;
    slots.set(dtf, s);
    return dtf;
}

function timeValue(x) {
    const t = x === undefined ? Date.now() : Number(x);
    if (!isFinite(t) || Math.abs(t) > 8.64e15) throw new RangeError('Invalid time value');
    return Math.trunc(t) + 0;
}
const dayPeriodText = (h, m) =>
    h === 12 && m === 0 ? 'noon' : h >= 6 && h < 12 ? 'in the morning' : h >= 12 && h < 18 ? 'in the afternoon' :
        h >= 18 && h < 21 ? 'in the evening' : 'at night';

function dtParts(s, t) {
    const o = s.f, F = fieldsAt(t, s.zone), P = [];
    const add = (type, value) => P.push({ type, value });
    const lit = (v) => add('literal', v);
    const text = o.month && o.month !== 'numeric' && o.month !== '2-digit';
    const wd = () => add('weekday', nameOf(DAYS, F.w, o.weekday));
    const yr = () => add('year', o.year === '2-digit' ? pad2(((F.y % 100) + 100) % 100) : String(F.y > 0 ? F.y : 1 - F.y));
    const mo = () => add('month', text ? nameOf(MONTHS, F.M, o.month) : o.month === '2-digit' ? pad2(F.M + 1) : String(F.M + 1));
    const dy = () => add('day', o.day === '2-digit' ? pad2(F.d) : String(F.d));
    const era = () => add('era', o.era === 'long' ? (F.y > 0 ? 'Anno Domini' : 'Before Christ') :
        o.era === 'narrow' ? (F.y > 0 ? 'A' : 'B') : (F.y > 0 ? 'AD' : 'BC'));
    /* the date */
    if (text) {
        if (o.weekday) {
            wd();
            if (o.month || o.day || o.year) lit(', ');
        }
        mo();
        if (o.day) {
            lit(' ');
            dy();
        }
        if (o.year) {
            lit(o.day ? ', ' : ' ');
            yr();
        }
        if (o.era) {
            lit(' ');
            era();
        }
    } else if (o.month || o.day || o.year) {
        if (o.weekday) {
            wd();
            lit(', ');
        }
        const seq = [];
        if (o.month) seq.push(mo);
        if (o.day) seq.push(dy);
        if (o.year) seq.push(yr);
        seq.forEach((fn, i) => {
            if (i) lit('/');
            fn();
        });
        if (o.era) {
            lit(' ');
            era();
        }
    } else if (o.weekday) {
        wd();
    } else if (o.era) {
        era();
    }
    const hasDate = P.length > 0;
    /* the time */
    const hasTime = o.hour || o.minute || o.second || o.fractionalSecondDigits || o.dayPeriod;
    if (hasTime && hasDate) lit(s.dateStyle === 'full' || s.dateStyle === 'long' || o.month === 'long' ? ' at ' : ', ');
    const h12 = s.hourCycle === 'h12' || s.hourCycle === 'h11';
    if (o.hour) {
        let h = F.h;
        if (s.hourCycle === 'h12') h = h % 12 || 12;
        else if (s.hourCycle === 'h11') h %= 12;
        else if (s.hourCycle === 'h24') h = h || 24;
        add('hour', o.hour === '2-digit' || !h12 ? pad2(h) : String(h));
    }
    if (o.minute) {
        if (o.hour) lit(':');
        add('minute', o.hour || o.second || o.minute === '2-digit' ? pad2(F.m) : String(F.m));
    }
    if (o.second) {
        if (o.hour || o.minute) lit(':');
        add('second', o.hour || o.minute || o.second === '2-digit' ? pad2(F.s) : String(F.s));
    }
    if (o.fractionalSecondDigits) {
        if (o.second) lit('.');
        add('fractionalSecond', String(F.ms).padStart(3, '0').slice(0, o.fractionalSecondDigits));
    }
    if (o.dayPeriod && (!o.hour || h12)) {
        if (o.hour || o.minute || o.second) lit(' ');
        add('dayPeriod', dayPeriodText(F.h, F.m));
    } else if (o.hour && h12) {
        lit(' ');
        add('dayPeriod', F.h < 12 ? 'AM' : 'PM');
    }
    if (o.timeZoneName) {
        if (P.length) lit(hasTime ? ' ' : ', ');
        add('timeZoneName', zoneName(s.zone, F, o.timeZoneName));
    }
    return P;
}
const dtFormat = (s, x) => join(dtParts(s, timeValue(x)));

function DateTimeFormat(locales, options) {
    if (!new.target) return new DateTimeFormat(locales, options);
    initDateTimeFormat(this, locales, options, 'any', 'date');
}
function rangeParts(s, a, b) {
    if (a === undefined || b === undefined) throw new TypeError('startDate or endDate is undefined');
    const x = dtParts(s, timeValue(a)), y = dtParts(s, timeValue(b));
    if (join(x) === join(y)) return x.map((p) => ({ type: p.type, value: p.value, source: 'shared' }));
    return x.map((p) => ({ type: p.type, value: p.value, source: 'startRange' }))
        .concat([{ type: 'literal', value: THIN + '–' + THIN, source: 'shared' }],
                y.map((p) => ({ type: p.type, value: p.value, source: 'endRange' })));
}
methods(DateTimeFormat.prototype, {
    get format() {
        const s = inner(this, 'DateTimeFormat', 'format');
        if (!s.bound) {
            const f = (x) => dtFormat(s, x);
            Object.defineProperty(f, 'name', { value: '' });
            s.bound = f;
        }
        return s.bound;
    },
    formatToParts(x) {
        return dtParts(inner(this, 'DateTimeFormat', 'formatToParts'), timeValue(x));
    },
    formatRange(a, b) {
        return join(rangeParts(inner(this, 'DateTimeFormat', 'formatRange'), a, b));
    },
    formatRangeToParts(a, b) {
        return rangeParts(inner(this, 'DateTimeFormat', 'formatRangeToParts'), a, b);
    },
    resolvedOptions() {
        const s = inner(this, 'DateTimeFormat', 'resolvedOptions');
        const f = s.f;
        const r = { locale: s.locale, calendar: s.calendar, numberingSystem: s.numberingSystem, timeZone: s.zone.id };
        if (s.hourCycle) {
            r.hourCycle = s.hourCycle;
            r.hour12 = s.hourCycle === 'h11' || s.hourCycle === 'h12';
        }
        if (!s.dateStyle && !s.timeStyle) {
            const h12 = r.hour12;
            for (const [name] of DT_FIELDS) {
                let v = f[name];
                if (v === undefined) continue;
                if (name === 'hour' && !h12) v = '2-digit';
                if (name === 'minute' && f.hour) v = '2-digit';
                if (name === 'second' && (f.hour || f.minute)) v = '2-digit';
                r[name] = v;
            }
        } else {
            if (s.dateStyle) r.dateStyle = s.dateStyle;
            if (s.timeStyle) r.timeStyle = s.timeStyle;
        }
        return r;
    },
});
DateTimeFormat.supportedLocalesOf = supportedLocalesOf;
tagged(DateTimeFormat, 'DateTimeFormat');

/* ---------------- RelativeTimeFormat ---------------- */

const RT_UNITS = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];
const RT_SHORT = { second: 'sec.', minute: 'min.', hour: 'hr.', day: 'day|days', week: 'wk.', month: 'mo.',
                   quarter: 'qtr.|qtrs.', year: 'yr.' };
const RT_NARROW = { second: 's', minute: 'm', hour: 'h', day: 'd', week: 'w', month: 'mo', quarter: 'q', year: 'y' };
const RT_AUTO = {
    day: ['yesterday', 'today', 'tomorrow'], second: [null, 'now', null], minute: [null, 'this minute', null],
    hour: [null, 'this hour', null],
};
function rtParts(s, value, unit) {
    let v = Number(value);
    unit = String(unit);
    if (!isFinite(v)) throw new RangeError('Invalid time value: ' + v);
    const u = unit.endsWith('s') ? unit.slice(0, -1) : unit;
    if (!RT_UNITS.includes(u)) throw new RangeError('Invalid unit argument for format() \'' + unit + '\'');
    if (s.numeric === 'auto') {
        let w = RT_AUTO[u];
        if (!w && v >= -1 && v <= 1 && Number.isInteger(v)) {
            const sh = s.style === 'long' ? u : RT_SHORT[u].split('|')[0];
            w = ['last ' + sh, 'this ' + sh, 'next ' + sh];
        }
        if (w && Number.isInteger(v) && v >= -1 && v <= 1 && w[v + 1]) return [{ type: 'literal', value: w[v + 1] }];
    }
    const past = v < 0 || Object.is(v, -0);
    const num = numParts(s.nf, Math.abs(v), u);
    const one = num.length === 1 && num[0].value === '1';
    let word;
    if (s.style === 'narrow') word = RT_NARROW[u];
    else if (s.style === 'short') {
        const e = RT_SHORT[u].split('|');
        word = ' ' + (one ? e[0] : e[1] || e[0]);
    } else {
        word = ' ' + u + (one ? '' : 's');
    }
    return past ? [...num, { type: 'literal', value: word + ' ago' }] : [{ type: 'literal', value: 'in ' }, ...num,
                                                                        { type: 'literal', value: word }];
}
class RelativeTimeFormat {
    constructor(locales, options) {
        const { o } = startLocale(locales, options);
        const s = { kind: 'RelativeTimeFormat', locale: LOCALE, numberingSystem: 'latn' };
        getStr(o, 'numberingSystem', null, undefined);
        s.style = getStr(o, 'style', ['long', 'short', 'narrow'], 'long');
        s.numeric = getStr(o, 'numeric', ['always', 'auto'], 'always');
        s.nf = slots.get(initNumberFormat({}, undefined, undefined));
        slots.set(this, s);
    }
    format(value, unit) {
        return join(rtParts(inner(this, 'RelativeTimeFormat', 'format'), value, unit));
    }
    formatToParts(value, unit) {
        return rtParts(inner(this, 'RelativeTimeFormat', 'formatToParts'), value, unit);
    }
    resolvedOptions() {
        const s = inner(this, 'RelativeTimeFormat', 'resolvedOptions');
        return { locale: s.locale, style: s.style, numeric: s.numeric, numberingSystem: s.numberingSystem };
    }
    static supportedLocalesOf(l, o) {
        return supportedLocalesOf(l, o);
    }
}
tagged(RelativeTimeFormat, 'RelativeTimeFormat');

/* ---------------- ListFormat ---------------- */

class ListFormat {
    constructor(locales, options) {
        const { o } = startLocale(locales, options);
        const s = { kind: 'ListFormat', locale: LOCALE };
        s.type = getStr(o, 'type', ['conjunction', 'disjunction', 'unit'], 'conjunction');
        s.style = getStr(o, 'style', ['long', 'short', 'narrow'], 'long');
        slots.set(this, s);
    }
    format(list) {
        return join(listParts(inner(this, 'ListFormat', 'format'), list));
    }
    formatToParts(list) {
        return listParts(inner(this, 'ListFormat', 'formatToParts'), list);
    }
    resolvedOptions() {
        const s = inner(this, 'ListFormat', 'resolvedOptions');
        return { locale: s.locale, type: s.type, style: s.style };
    }
    static supportedLocalesOf(l, o) {
        return supportedLocalesOf(l, o);
    }
}
function listParts(s, list) {
    const a = [];
    if (list !== undefined) {
        for (const v of list) {
            if (typeof v !== 'string') throw new TypeError('Iterable yielded ' + String(v) + ' which is not a string');
            a.push(v);
        }
    }
    let mid = ', ', two, end;
    if (s.type === 'unit') {
        if (s.style === 'narrow') mid = ' ';
        two = end = mid;
    } else if (s.type === 'disjunction') {
        two = ' or ';
        end = ', or ';
    } else if (s.style === 'long') {
        two = ' and ';
        end = ', and ';
    } else if (s.style === 'short') {
        two = ' & ';
        end = ', & ';
    } else {
        two = end = ', ';
    }
    const P = [];
    a.forEach((v, i) => {
        if (i) P.push({ type: 'literal', value: a.length === 2 ? two : i === a.length - 1 ? end : mid });
        P.push({ type: 'element', value: v });
    });
    return P;
}
tagged(ListFormat, 'ListFormat');

/* ---------------- Collator ---------------- */

const LETTER = /\p{L}/u, NUMBER = /\p{Nd}/u, SPACE = /\s/u, MARK = /\p{M}/u, PUNCT = /[\p{P}\p{Z}\s]/u;
const isAscii = (str) => /^[\x00-\x7f]*$/.test(str);
/* primary weight of a code point: class (space, other, digit, letter), then the folded code point */
function weight(c) {
    if (c < 128) {
        if ((c >= 97 && c <= 122)) return 0x400000 + c;
        if (c >= 65 && c <= 90) return 0x400000 + c + 32;
        if (c >= 48 && c <= 57) return 0x300000 + c;
        if (c === 32 || (c >= 9 && c <= 13)) return 0x100000 + c;
        return 0x200000 + c;
    }
    const ch = String.fromCodePoint(c);
    if (LETTER.test(ch)) return 0x400000 + ch.toLowerCase().codePointAt(0);
    if (NUMBER.test(ch)) return 0x300000 + c;
    if (SPACE.test(ch)) return 0x100000 + c;
    return 0x200000 + c;
}
/* a sort compares each string many times, so the decomposed forms of the
 * last few hundred strings are kept */
const prepCache = [new Map(), new Map()];
function prep(str, s) {
    const cache = prepCache[s.ignorePunctuation ? 1 : 0];
    let r = cache.get(str);
    if (r !== undefined) return r;
    r = isAscii(str) ? str : str.normalize('NFD');
    if (s.ignorePunctuation) r = r.replace(/[\p{P}\p{Z}\s]+/gu, '');
    if (cache.size >= 512) cache.clear();
    if (str.length <= 256) cache.set(str, r);
    return r;
}
const isMark = (c) => c >= 0x300 && (c <= 0x36f || MARK.test(String.fromCodePoint(c)));
const ASCII_W = [];
for (let c = 0; c < 128; c++) ASCII_W.push(weight(c));
/* compares two strings whose characters up to the first difference are
 * ASCII; null when a character that is not ASCII is reached first */
function compareAscii(s, a, b) {
    const n = a.length, m = b.length;
    let i = 0;
    while (i < n && i < m && a.charCodeAt(i) === b.charCodeAt(i)) i++;
    if (s.numeric) while (i > 0 && a.charCodeAt(i - 1) >= 48 && a.charCodeAt(i - 1) <= 57) i--;
    let j = i;
    while (i < n && j < m) {
        const ca = a.charCodeAt(i), cb = b.charCodeAt(j);
        if (ca > 127 || cb > 127) return null;
        if (s.numeric && ca >= 48 && ca <= 57 && cb >= 48 && cb <= 57) return null;
        if (ca !== cb) {
            const wa = ASCII_W[ca], wb = ASCII_W[cb];
            if (wa !== wb) return wa < wb ? -1 : 1;
        }
        i++;
        j++;
    }
    if (i < n || j < m) {
        const c = i < n ? a.charCodeAt(i) : b.charCodeAt(j);
        if (c > 127) return null;
        return i < n ? 1 : -1;
    }
    if (s.sensitivity === 'base' || s.sensitivity === 'accent') return 0;
    for (let k = 0; k < n; k++) {
        const x = a.charCodeAt(k), y = b.charCodeAt(k);
        if (x !== y) return (x >= 97 ? -1 : 1) * (s.caseFirst === 'upper' ? -1 : 1);
    }
    return 0;
}
function compareStrings(s, a, b) {
    a = String(a);
    b = String(b);
    if (a === b) return 0;
    if (!s.ignorePunctuation) {
        const r = compareAscii(s, a, b);
        if (r !== null) return r;
    }
    a = prep(a, s);
    b = prep(b, s);
    if (a === b) return 0;
    /* primary: base letters, folded. An identical prefix differs at no
     * level, so the walk starts after it (at a digit run's start when
     * digits compare as numbers, and never inside a surrogate pair). */
    let k = 0;
    while (k < a.length && k < b.length && a.charCodeAt(k) === b.charCodeAt(k)) k++;
    if (k > 0 && (a.charCodeAt(k - 1) & 0xfc00) === 0xd800) k--;
    if (s.numeric) while (k > 0 && a.charCodeAt(k - 1) >= 48 && a.charCodeAt(k - 1) <= 57) k--;
    let i = k, j = k;
    const cp = (str, x) => {
        const c = str.charCodeAt(x);
        return (c & 0xfc00) === 0xd800 ? str.codePointAt(x) : c;
    };
    for (;;) {
        let ca = i < a.length ? cp(a, i) : -1, cb = j < b.length ? cp(b, j) : -1;
        while (ca >= 0x300 && isMark(ca)) {
            i += ca > 0xffff ? 2 : 1;
            ca = i < a.length ? cp(a, i) : -1;
        }
        while (cb >= 0x300 && isMark(cb)) {
            j += cb > 0xffff ? 2 : 1;
            cb = j < b.length ? cp(b, j) : -1;
        }
        if (ca < 0 || cb < 0) {
            if (ca >= 0) return 1;
            if (cb >= 0) return -1;
            break;
        }
        if (s.numeric && ca >= 48 && ca <= 57 && cb >= 48 && cb <= 57) {
            let ei = i, ej = j;
            while (ei < a.length && a.charCodeAt(ei) >= 48 && a.charCodeAt(ei) <= 57) ei++;
            while (ej < b.length && b.charCodeAt(ej) >= 48 && b.charCodeAt(ej) <= 57) ej++;
            const na = a.slice(i, ei).replace(/^0+(?=\d)/, ''), nb = b.slice(j, ej).replace(/^0+(?=\d)/, '');
            if (na.length !== nb.length) return na.length < nb.length ? -1 : 1;
            if (na !== nb) return na < nb ? -1 : 1;
            i = ei;
            j = ej;
            continue;
        }
        if (ca !== cb) {
            const wa = ca < 128 ? ASCII_W[ca] : weight(ca), wb = cb < 128 ? ASCII_W[cb] : weight(cb);
            if (wa !== wb) return wa < wb ? -1 : 1;
        }
        i += ca > 0xffff ? 2 : 1;
        j += cb > 0xffff ? 2 : 1;
    }
    if (s.sensitivity === 'base') return 0;
    const la = a.toLowerCase(), lb = b.toLowerCase();
    /* secondary: accents */
    if (s.sensitivity !== 'case' && la !== lb) return la < lb ? -1 : 1;
    if (s.sensitivity === 'accent') return 0;
    /* tertiary: case, lower case first unless caseFirst is upper */
    const n = Math.min(a.length, b.length);
    for (let k = 0; k < n; k++) {
        const x = a[k], y = b[k];
        if (x === y) continue;
        if (x.toLowerCase() !== y.toLowerCase()) continue;
        const lower = x === x.toLowerCase();
        return (lower ? -1 : 1) * (s.caseFirst === 'upper' ? -1 : 1);
    }
    return 0;
}
function Collator(locales, options) {
    if (!new.target) return new Collator(locales, options);
    const { list, o } = startLocale(locales, options);
    const s = { kind: 'Collator', locale: LOCALE, bound: undefined };
    s.usage = getStr(o, 'usage', ['sort', 'search'], 'sort');
    getStr(o, 'collation', null, undefined);
    const num = o.numeric === undefined ? undefined : Boolean(o.numeric);
    const cf = getStr(o, 'caseFirst', ['upper', 'lower', 'false'], undefined);
    s.numeric = num !== undefined ? num : uKey(list, 'kn') === 'true';
    s.caseFirst = cf !== undefined ? cf : 'false';
    s.sensitivity = getStr(o, 'sensitivity', ['base', 'accent', 'case', 'variant'], 'variant');
    s.ignorePunctuation = getBool(o, 'ignorePunctuation', false);
    slots.set(this, s);
}
methods(Collator.prototype, {
    get compare() {
        const s = inner(this, 'Collator', 'compare');
        if (!s.bound) {
            const f = (a, b) => compareStrings(s, a, b);
            Object.defineProperty(f, 'name', { value: '' });
            s.bound = f;
        }
        return s.bound;
    },
    resolvedOptions() {
        const s = inner(this, 'Collator', 'resolvedOptions');
        return { locale: s.locale, usage: s.usage, sensitivity: s.sensitivity, ignorePunctuation: s.ignorePunctuation,
                 collation: 'default', numeric: s.numeric, caseFirst: s.caseFirst };
    },
});
Collator.supportedLocalesOf = supportedLocalesOf;
tagged(Collator, 'Collator');

/* ---------------- Segmenter ---------------- */

const GRAPHEME_RE = /\r\n|[\u{1F1E6}-\u{1F1FF}]{2}|[\s\S](?:[\p{M}\u200C\uFE00-\uFE0F\u{1F3FB}-\u{1F3FF}\u{E0020}-\u{E007F}]|\u200D[\s\S])*/gu;
const WORD_RE = /[\p{L}\p{N}\p{M}_]+(?:(?:['\u2019.:\u00B7]|(?<=\p{N}),(?=\p{N}))[\p{L}\p{N}\p{M}_]+)*|[^\S\r\n\u2028\u2029]+|\r\n|[\s\S]/gu;
const SENTENCE_RE = /[^.!?\u3002]+(?:[.!?\u3002]+["'\u201D\u2019)\]]*)?\s*|[.!?\u3002]+["'\u201D\u2019)\]]*\s*/gu;
const WORDLIKE = /^[\p{L}\p{N}]/u;

function segmentList(s, str) {
    const re = s.granularity === 'word' ? WORD_RE : s.granularity === 'sentence' ? SENTENCE_RE : GRAPHEME_RE;
    re.lastIndex = 0;
    const out = [];
    let at = 0, m;
    while (at < str.length && (m = re.exec(str)) !== null) {
        if (m.index !== at || !m[0]) {
            /* not expected; take one code unit so the loop ends */
            const one = str[at];
            out.push({ segment: one, index: at });
            at += 1;
            re.lastIndex = at;
            continue;
        }
        out.push({ segment: m[0], index: at });
        at += m[0].length;
    }
    return out;
}
function segData(s, seg, input) {
    const d = { segment: seg.segment, index: seg.index, input };
    if (s.granularity === 'word') d.isWordLike = WORDLIKE.test(seg.segment);
    return d;
}
class Segments {
    containing(index) {
        const z = slots.get(this);
        if (!z || z.kind !== 'Segments') throw new TypeError('incompatible receiver');
        let n = Math.trunc(Number(index)) || 0;
        if (n < 0 || n >= z.input.length) return undefined;
        if (!z.list) z.list = segmentList(z.s, z.input);
        let lo = 0, hi = z.list.length - 1;
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1;
            if (z.list[mid].index <= n) lo = mid;
            else hi = mid - 1;
        }
        return segData(z.s, z.list[lo], z.input);
    }
    [Symbol.iterator]() {
        const z = slots.get(this);
        if (!z || z.kind !== 'Segments') throw new TypeError('incompatible receiver');
        if (!z.list) z.list = segmentList(z.s, z.input);
        let k = 0;
        const it = Object.create(SegmentIterator);
        it.next = () => (k < z.list.length ? { value: segData(z.s, z.list[k++], z.input), done: false } :
            { value: undefined, done: true });
        return it;
    }
}
const SegmentIterator = Object.create(Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())), {
    [Symbol.toStringTag]: { value: 'Segmenter String Iterator', configurable: true },
});
class Segmenter {
    constructor(locales, options) {
        const { o } = startLocale(locales, options);
        const s = { kind: 'Segmenter', locale: LOCALE };
        s.granularity = getStr(o, 'granularity', ['grapheme', 'word', 'sentence'], 'grapheme');
        slots.set(this, s);
    }
    segment(input) {
        const s = inner(this, 'Segmenter', 'segment');
        const seg = new Segments();
        slots.set(seg, { kind: 'Segments', s, input: String(input), list: null });
        return seg;
    }
    resolvedOptions() {
        const s = inner(this, 'Segmenter', 'resolvedOptions');
        return { locale: s.locale, granularity: s.granularity };
    }
    static supportedLocalesOf(l, o) {
        return supportedLocalesOf(l, o);
    }
}
tagged(Segmenter, 'Segmenter');

/* ---------------- DisplayNames ---------------- */

const NAMES = {
    language: { en: 'English', 'en-US': 'American English', 'en-GB': 'British English', fr: 'French', de: 'German',
                es: 'Spanish', it: 'Italian', pt: 'Portuguese', nl: 'Dutch', sv: 'Swedish', da: 'Danish',
                no: 'Norwegian', nb: 'Norwegian Bokmål', fi: 'Finnish', pl: 'Polish', cs: 'Czech', ru: 'Russian',
                uk: 'Ukrainian', el: 'Greek', tr: 'Turkish', ar: 'Arabic', he: 'Hebrew', hi: 'Hindi', ja: 'Japanese',
                ko: 'Korean', zh: 'Chinese', 'zh-Hans': 'Simplified Chinese', 'zh-Hant': 'Traditional Chinese',
                vi: 'Vietnamese', th: 'Thai', id: 'Indonesian', hu: 'Hungarian', ro: 'Romanian' },
    region: { US: 'United States', GB: 'United Kingdom', CA: 'Canada', AU: 'Australia', NZ: 'New Zealand',
              IE: 'Ireland', FR: 'France', DE: 'Germany', ES: 'Spain', IT: 'Italy', PT: 'Portugal', NL: 'Netherlands',
              BE: 'Belgium', CH: 'Switzerland', AT: 'Austria', SE: 'Sweden', NO: 'Norway', DK: 'Denmark',
              FI: 'Finland', PL: 'Poland', RU: 'Russia', UA: 'Ukraine', JP: 'Japan', KR: 'South Korea', CN: 'China',
              TW: 'Taiwan', HK: 'Hong Kong SAR China', IN: 'India', BR: 'Brazil', MX: 'Mexico', AR: 'Argentina',
              ZA: 'South Africa', 419: 'Latin America', '001': 'world', 150: 'Europe' },
    script: { Latn: 'Latin', Cyrl: 'Cyrillic', Grek: 'Greek', Arab: 'Arabic', Hebr: 'Hebrew', Hans: 'Simplified Han',
              Hant: 'Traditional Han', Jpan: 'Japanese', Kore: 'Korean', Deva: 'Devanagari', Thai: 'Thai' },
};
class DisplayNames {
    constructor(locales, options) {
        if (options === undefined) throw new TypeError('Required option type is missing');
        const { o } = startLocale(locales, options);
        const s = { kind: 'DisplayNames', locale: LOCALE };
        s.style = getStr(o, 'style', ['narrow', 'short', 'long'], 'long');
        s.type = getStr(o, 'type', ['language', 'region', 'script', 'currency', 'calendar', 'dateTimeField'], undefined);
        if (s.type === undefined) throw new TypeError('Required option type is missing');
        s.fallback = getStr(o, 'fallback', ['code', 'none'], 'code');
        s.languageDisplay = getStr(o, 'languageDisplay', ['dialect', 'standard'], 'dialect');
        slots.set(this, s);
    }
    of(code) {
        const s = inner(this, 'DisplayNames', 'of');
        code = String(code);
        let key = code, name;
        if (s.type === 'language') key = canonTag(code);
        else if (s.type === 'region') key = code.toUpperCase();
        else if (s.type === 'script') key = code.length === 4 ? code[0].toUpperCase() + code.slice(1).toLowerCase() : code;
        else if (s.type === 'currency') key = code.toUpperCase();
        if (s.type === 'currency') name = CURRENCY[key] ? CURRENCY[key][1].replace(/\b[a-z]/g, (c) => c.toUpperCase()) : undefined;
        else if (NAMES[s.type]) {
            name = NAMES[s.type][key];
            if (name === undefined && s.type === 'language') {
                const p = key.split('-');
                const base = NAMES.language[p[0]];
                if (base && p.length > 1) {
                    const extra = p.slice(1).map((x) => NAMES.script[x] || NAMES.region[x]).filter(Boolean);
                    if (extra.length === p.length - 1) name = base + ' (' + extra.join(', ') + ')';
                }
            }
        }
        if (name !== undefined) return name;
        return s.fallback === 'code' ? key : undefined;
    }
    resolvedOptions() {
        const s = inner(this, 'DisplayNames', 'resolvedOptions');
        const r = { locale: s.locale, style: s.style, type: s.type, fallback: s.fallback };
        if (s.type === 'language') r.languageDisplay = s.languageDisplay;
        return r;
    }
    static supportedLocalesOf(l, o) {
        return supportedLocalesOf(l, o);
    }
}
tagged(DisplayNames, 'DisplayNames');

/* ---------------- Locale ---------------- */

const LIKELY = { en: 'en-Latn-US', fr: 'fr-Latn-FR', de: 'de-Latn-DE', es: 'es-Latn-ES', ja: 'ja-Jpan-JP', zh: 'zh-Hans-CN' };
function parseTag(tag) {
    const p = tag.split('-');
    const r = { language: p[0], script: undefined, region: undefined, variants: [], ext: '' };
    let i = 1;
    if (p[i] && p[i].length === 4 && !/\d/.test(p[i])) r.script = p[i++];
    if (p[i] && (p[i].length === 2 || /^\d{3}$/.test(p[i]))) r.region = p[i++];
    while (p[i] && p[i].length > 1) r.variants.push(p[i++]);
    if (i < p.length) r.ext = '-' + p.slice(i).join('-');
    return r;
}
class Locale {
    constructor(tag, options) {
        if (typeof tag !== 'string' && (tag === null || typeof tag !== 'object'))
            throw new TypeError('First argument to Intl.Locale constructor can\'t be empty or missing');
        let t = canonTag(tag);
        const o = toObj(options);
        const r = parseTag(t);
        const lang = getStr(o, 'language', null, undefined), script = getStr(o, 'script', null, undefined),
            region = getStr(o, 'region', null, undefined);
        if (lang !== undefined) r.language = canonTag(lang).split('-')[0];
        if (script !== undefined) r.script = script[0].toUpperCase() + script.slice(1).toLowerCase();
        if (region !== undefined) r.region = region.toUpperCase();
        const kw = {};
        for (const [k, name] of [['ca', 'calendar'], ['co', 'collation'], ['hc', 'hourCycle'], ['kf', 'caseFirst'],
                                 ['kn', 'numeric'], ['nu', 'numberingSystem']]) {
            let v = name === 'numeric' ? (o.numeric === undefined ? undefined : String(Boolean(o.numeric))) :
                getStr(o, name, name === 'hourCycle' ? ['h11', 'h12', 'h23', 'h24'] :
                    name === 'caseFirst' ? ['upper', 'lower', 'false'] : null, undefined);
            if (v === undefined) {
                const m = new RegExp('-u(?:-[a-z\\d]{2,8})*?-' + k + '(?:-([a-z\\d]{3,8}))?(?=-|$)').exec(t);
                if (m) v = m[1] || 'true';
            }
            if (v !== undefined) kw[name] = v;
        }
        const base = [r.language, r.script, r.region, ...r.variants].filter(Boolean).join('-');
        let ext = r.ext.replace(/-u(?:-[a-z\d]{2,8})*/, '');
        const keys = [['ca', 'calendar'], ['co', 'collation'], ['hc', 'hourCycle'], ['kf', 'caseFirst'], ['kn', 'numeric'],
                      ['nu', 'numberingSystem']].filter((e) => kw[e[1]] !== undefined)
            .map((e) => '-' + e[0] + (kw[e[1]] === 'true' && e[0] === 'kn' ? '' : '-' + kw[e[1]]));
        t = base + (keys.length ? '-u' + keys.join('') : '') + ext;
        slots.set(this, Object.assign({ kind: 'Locale', tag: t, base }, r, kw));
    }
    toString() { return inner(this, 'Locale', 'toString').tag; }
    toJSON() { return this.toString(); }
    get baseName() { return inner(this, 'Locale', 'baseName').base; }
    get language() { return inner(this, 'Locale', 'language').language; }
    get script() { return inner(this, 'Locale', 'script').script; }
    get region() { return inner(this, 'Locale', 'region').region; }
    get calendar() { return inner(this, 'Locale', 'calendar').calendar; }
    get collation() { return inner(this, 'Locale', 'collation').collation; }
    get hourCycle() { return inner(this, 'Locale', 'hourCycle').hourCycle; }
    get caseFirst() { return inner(this, 'Locale', 'caseFirst').caseFirst; }
    get numeric() { return inner(this, 'Locale', 'numeric').numeric === 'true'; }
    get numberingSystem() { return inner(this, 'Locale', 'numberingSystem').numberingSystem; }
    maximize() {
        const s = inner(this, 'Locale', 'maximize');
        const l = parseTag(LIKELY[s.language] || s.language);
        const base = [s.language, s.script || l.script, s.region || l.region, ...s.variants].filter(Boolean).join('-');
        return new Locale(base + s.tag.slice(s.base.length));
    }
    minimize() {
        const s = inner(this, 'Locale', 'minimize');
        const l = parseTag(LIKELY[s.language] || s.language);
        const base = [s.language, s.script === l.script ? undefined : s.script, s.region === l.region ? undefined : s.region,
                      ...s.variants].filter(Boolean).join('-');
        return new Locale(base + s.tag.slice(s.base.length));
    }
    getCalendars() { inner(this, 'Locale', 'getCalendars'); return ['gregory']; }
    getCollations() { inner(this, 'Locale', 'getCollations'); return ['emoji', 'eor']; }
    getHourCycles() { const s = inner(this, 'Locale', 'getHourCycles'); return [s.hourCycle || 'h12']; }
    getNumberingSystems() { inner(this, 'Locale', 'getNumberingSystems'); return ['latn']; }
    getTimeZones() { const s = inner(this, 'Locale', 'getTimeZones'); return s.region === 'US' ? Object.keys(ZONES).filter((z) => ZONES[z][2] && !/Toronto|Vancouver|Edmonton|Winnipeg|Halifax|Mexico/.test(z)).sort() : undefined; }
    getTextInfo() { inner(this, 'Locale', 'getTextInfo'); return { direction: /^(?:ar|he|fa|ur|yi)$/.test(this.language) ? 'rtl' : 'ltr' }; }
    getWeekInfo() { inner(this, 'Locale', 'getWeekInfo'); return { firstDay: 7, weekend: [6, 7], minimalDays: 1 }; }
    get textInfo() { return this.getTextInfo(); }
    get weekInfo() { return this.getWeekInfo(); }
    get hourCycles() { return this.getHourCycles(); }
    get calendars() { return this.getCalendars(); }
    get timeZones() { return this.getTimeZones(); }
}
tagged(Locale, 'Locale');

/* ---------------- Intl ---------------- */

const Intl = {};
methods(Intl, {
    DateTimeFormat, NumberFormat, Collator, PluralRules, RelativeTimeFormat, ListFormat, Segmenter, DisplayNames, Locale,
    getCanonicalLocales(locales) {
        return localeList(locales);
    },
    supportedValuesOf(key) {
        key = String(key);
        switch (key) {
        case 'calendar': return ['gregory'];
        case 'collation': return ['default'];
        case 'currency': return Object.keys(CURRENCY).sort();
        case 'numberingSystem': return ['latn'];
        case 'timeZone': return Object.keys(ZONES).concat('UTC').sort();
        case 'unit': return Object.keys(UNITS).sort();
        }
        throw new RangeError('Invalid key : ' + key);
    },
});
Object.defineProperty(Intl, Symbol.toStringTag, { value: 'Intl', configurable: true });

/* ---------------- the locale methods of Number, Date and String ---------------- */

const cache = {};
function numberToLocale(x, locales, options) {
    if (locales === undefined && options === undefined)
        return numFormat(cache.nf || (cache.nf = slots.get(initNumberFormat({}, undefined, undefined))), x);
    return numFormat(slots.get(initNumberFormat({}, locales, options)), x);
}
/* kind: 'any' (toLocaleString), 'date' or 'time' */
function dateToLocale(t, locales, options, kind) {
    if (t !== t) return 'Invalid Date';
    const defaults = kind === 'any' ? 'all' : kind;
    if (locales === undefined && options === undefined) {
        const s = cache[kind] || (cache[kind] = slots.get(initDateTimeFormat({}, undefined, undefined, kind, defaults)));
        return dtFormat(s, t);
    }
    return dtFormat(slots.get(initDateTimeFormat({}, locales, options, kind, defaults)), t);
}
function compare(a, b, locales, options) {
    if (locales === undefined && options === undefined) {
        const s = cache.co || (cache.co = slots.get(new Collator()));
        return compareStrings(s, a, b);
    }
    return compareStrings(slots.get(new Collator(locales, options)), a, b);
}

return { Intl, numberToLocale, dateToLocale, compare };
})
