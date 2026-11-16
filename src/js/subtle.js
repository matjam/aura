/* Aura's crypto.subtle (the Web Cryptography API's SubtleCrypto and
 * CryptoKey). The build compiles this file to its own bytecode
 * (subtle.bc); the prelude reads it with N.subtle() the first time script
 * touches crypto.subtle, SubtleCrypto or CryptoKey, so a page that never
 * does pays only for the getters. It is a function of G (the global
 * object), N (the natives) and domErr, and returns the SubtleCrypto
 * object and the two classes.
 *
 * The cryptography is in C (src/js/natives_crypto.c over
 * src/net/webcrypto.c and the TLS library's PSA interface); this file
 * normalizes algorithms, enforces key types, usages and extractability as
 * the specification says, and converts the key formats: spki, pkcs8 and
 * the EC and RSA structures are read and written here (DER), and jwk.
 * docs/javascript.md ("crypto") lists what is covered. */
(function (G, N, domErr) {
'use strict';

/* ---------------- errors and bytes ---------------- */

const fail = (name, msg) => { throw domErr(msg, name); };
const dataErr = (msg) => fail('DataError', msg || 'The key data is not valid');
const opErr = (msg) => fail('OperationError', msg);
const accessErr = (msg) => fail('InvalidAccessError', msg);
const syntaxErr = (msg) => fail('SyntaxError', msg);
const notSupported = (msg) => fail('NotSupportedError', msg || 'The algorithm or operation is not supported');
const NATIVE = new Set(['DataError', 'OperationError', 'NotSupportedError', 'InvalidAccessError', 'SyntaxError']);
/* The natives throw plain Errors named after the DOMException. */
const conv = (e) => (e && !(e instanceof G.DOMException) && e instanceof Error && NATIVE.has(e.name) ? domErr(e.message, e.name) : e);
const run = (fn) => {
    let r;
    try { r = fn(); } catch (e) { return Promise.reject(conv(e)); }
    return Promise.resolve(r).then(undefined, (e) => { throw conv(e); });
};

const tagOf = (v) => Object.prototype.toString.call(v).slice(8, -1);
/* A copy of the bytes of a BufferSource, taken when the method is called. */
function bytes(d, what) {
    if (d instanceof ArrayBuffer || tagOf(d) === 'ArrayBuffer') return new Uint8Array(ArrayBuffer.prototype.slice.call(d, 0));
    if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer.slice(d.byteOffset, d.byteOffset + d.byteLength));
    throw new TypeError((what || 'The data') + ' is not an ArrayBuffer or a view of one');
}
const optBytes = (d, what) => (d === undefined ? undefined : bytes(d, what));
const concat = (...parts) => {
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
};
const u8 = (ab) => new Uint8Array(ab);
const hex = (b) => Array.from(b, (x) => (x < 16 ? '0' : '') + x.toString(16)).join('');
const unhex = (s) => new Uint8Array(s.match(/../g).map((h) => parseInt(h, 16)));
/* Leading zero bytes off (a DER integer's or a JWK value's magnitude). */
const strip = (b) => { let i = 0; while (i < b.length - 1 && b[i] === 0) i++; return b.subarray(i); };
const pad = (b, n) => { if (b.length > n) dataErr(); const o = new Uint8Array(n); o.set(b, n - b.length); return o; };
const bitLength = (b) => { b = strip(b); if (!b.length || !b[0]) return 0; return (b.length - 1) * 8 + (32 - Math.clz32(b[0])); };

/* base64url without padding (RFC 7515, appendix C) */
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
const B64REV = new Int16Array(128).fill(-1);
for (let i = 0; i < 64; i++) B64REV[B64.charCodeAt(i)] = i;
function b64u(b) {
    let s = '';
    let i = 0;
    for (; i + 2 < b.length; i += 3) {
        const n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        s += B64[n >> 18] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
    }
    if (b.length - i === 1) {
        const n = b[i] << 16;
        s += B64[n >> 18] + B64[(n >> 12) & 63];
    } else if (b.length - i === 2) {
        const n = (b[i] << 16) | (b[i + 1] << 8);
        s += B64[n >> 18] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63];
    }
    return s;
}
function unb64u(s, what) {
    if (typeof s !== 'string') dataErr('The JWK member ' + what + ' is not a string');
    if (s.length % 4 === 1) dataErr('The JWK member ' + what + ' is not base64url');
    const out = new Uint8Array(Math.floor(s.length * 3 / 4));
    let acc = 0, bits = 0, o = 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        const v = c < 128 ? B64REV[c] : -1;
        if (v < 0) dataErr('The JWK member ' + what + ' is not base64url');
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 255; }
    }
    if (acc & ((1 << bits) - 1)) dataErr('The JWK member ' + what + ' is not base64url');
    return out.subarray(0, o);
}

/* ---------------- DER ---------------- */

/* One element at pos: { tag, at, start, end } (where it begins, and the
 * start and end of its contents). Only definite, minimal lengths; nothing
 * past end. */
function der(b, pos, end) {
    const at = pos;
    if (pos + 2 > end) dataErr('The DER data is truncated');
    const tag = b[pos++];
    if ((tag & 0x1f) === 0x1f) dataErr('The DER data has a tag that is not supported');
    let len = b[pos++];
    if (len & 0x80) {
        const n = len & 0x7f;
        if (n === 0 || n > 3 || pos + n > end || b[pos] === 0) dataErr('The DER data has a bad length');
        len = 0;
        for (let i = 0; i < n; i++) len = len * 256 + b[pos++];
        if (len < 128) dataErr('The DER data has a bad length');
    }
    if (len > end - pos) dataErr('The DER data is truncated');
    return { tag, at, start: pos, end: pos + len };
}
function derExpect(b, pos, end, tag) {
    const e = der(b, pos, end);
    if (e.tag !== tag) dataErr('The DER data is not the structure expected');
    return e;
}
/* The children of a constructed element. */
function derKids(b, e) {
    const out = [];
    for (let p = e.start; p < e.end;) {
        const k = der(b, p, e.end);
        out.push(k);
        p = k.end;
        if (out.length > 64) dataErr();
    }
    return out;
}
const body = (b, e) => b.subarray(e.start, e.end);
/* A non-negative INTEGER's magnitude. */
function derUint(b, e) {
    if (e.tag !== 2 || e.end === e.start || b[e.start] & 0x80) dataErr('The DER integer is not valid');
    return strip(body(b, e));
}
function derLen(n) {
    if (n < 128) return [n];
    const out = [];
    while (n) { out.unshift(n & 255); n = Math.floor(n / 256); }
    return [0x80 | out.length, ...out];
}
const tlv = (tag, content) => concat(Uint8Array.of(tag, ...derLen(content.length)), content);
const seq = (...kids) => tlv(0x30, concat(...kids));
const derInt = (mag) => { mag = strip(mag); return tlv(2, mag[0] & 0x80 ? concat(Uint8Array.of(0), mag) : mag); };
const bitString = (b) => tlv(3, concat(Uint8Array.of(0), b));
const octets = (b) => tlv(4, b);
const DER_NULL = Uint8Array.of(5, 0);

const OID = {
    rsa: unhex('06092a864886f70d010101'),
    ec: unhex('06072a8648ce3d0201'),
    p256: unhex('06082a8648ce3d030107'),
    p384: unhex('06052b81040022'),
    p521: unhex('06052b81040023'),
    x25519: unhex('06032b656e'),
};
const CURVES = {
    'P-256': { bits: 256, bytes: 32, oid: OID.p256, jws: 'ES256' },
    'P-384': { bits: 384, bytes: 48, oid: OID.p384, jws: 'ES384' },
    'P-521': { bits: 521, bytes: 66, oid: OID.p521, jws: 'ES512' },
};
const curveOfOid = (o) => Object.keys(CURVES).find((c) => hex(CURVES[c].oid) === o);

/* AlgorithmIdentifier: the OID's bytes (tag and length included, as hex)
 * and its parameters' element. */
function algId(b, e) {
    if (e.tag !== 0x30) dataErr();
    const k = derKids(b, e);
    if (!k.length || k[0].tag !== 6 || k.length > 2) dataErr('The key data names no algorithm');
    return { oid: hex(b.subarray(k[0].at, k[0].end)), params: k[1] };
}
const elemHex = (b, e) => hex(b.subarray(e.at, e.end));
/* SubjectPublicKeyInfo -> { oid, params, key } */
function readSpki(b) {
    const top = der(b, 0, b.length);
    if (top.tag !== 0x30 || top.end !== b.length) dataErr('The key data is not a SubjectPublicKeyInfo');
    const k = derKids(b, top);
    if (k.length !== 2 || k[1].tag !== 3) dataErr('The key data is not a SubjectPublicKeyInfo');
    const a = algId(b, k[0]);
    if (k[1].end - k[1].start < 1 || b[k[1].start] !== 0) dataErr('The public key bit string is not valid');
    return { oid: a.oid, params: a.params, b, key: b.subarray(k[1].start + 1, k[1].end) };
}
/* PrivateKeyInfo (PKCS #8, version 1 or 2) -> { oid, params, key } */
function readPkcs8(b) {
    const top = der(b, 0, b.length);
    if (top.tag !== 0x30 || top.end !== b.length) dataErr('The key data is not a PrivateKeyInfo');
    const k = derKids(b, top);
    if (k.length < 3 || k.length > 5 || k[2].tag !== 4) dataErr('The key data is not a PrivateKeyInfo');
    const v = derUint(b, k[0]);
    if (v.length !== 1 || v[0] > 1) dataErr('The PrivateKeyInfo version is not supported');
    for (let i = 3; i < k.length; i++)
        if (k[i].tag !== 0xa0 && k[i].tag !== 0x81 && k[i].tag !== 0xa1) dataErr('The key data is not a PrivateKeyInfo');
    const a = algId(b, k[1]);
    return { oid: a.oid, params: a.params, b, key: body(b, k[2]) };
}
const writeSpki = (alg, key) => seq(alg, bitString(key));
const writePkcs8 = (alg, key) => seq(derInt(Uint8Array.of(0)), alg, octets(key));

/* RSAPublicKey -> { n, e } and RSAPrivateKey -> its nine integers */
function readRsaPublic(b) {
    const top = der(b, 0, b.length);
    if (top.tag !== 0x30 || top.end !== b.length) dataErr();
    const k = derKids(b, top);
    if (k.length !== 2) dataErr();
    return { n: derUint(b, k[0]), e: derUint(b, k[1]) };
}
const RSA_PRIV = ['n', 'e', 'd', 'p', 'q', 'dp', 'dq', 'qi'];
function readRsaPrivate(b) {
    const top = der(b, 0, b.length);
    if (top.tag !== 0x30 || top.end !== b.length) dataErr();
    const k = derKids(b, top);
    if (k.length !== 9) dataErr('Multi-prime RSA keys are not supported');
    const v = derUint(b, k[0]);
    if (v.length !== 1 || v[0] !== 0) dataErr('Multi-prime RSA keys are not supported');
    const out = {};
    RSA_PRIV.forEach((name, i) => { out[name] = derUint(b, k[i + 1]); });
    return out;
}
const writeRsaPublic = (j) => seq(derInt(j.n), derInt(j.e));
const writeRsaPrivate = (j) => seq(derInt(Uint8Array.of(0)), ...RSA_PRIV.map((n) => derInt(j[n])));

/* ---------------- algorithm names and parameters ---------------- */

const NAMES = ['RSASSA-PKCS1-v1_5', 'RSA-PSS', 'RSA-OAEP', 'ECDSA', 'ECDH', 'AES-CTR', 'AES-CBC', 'AES-GCM', 'AES-KW',
    'HMAC', 'SHA-1', 'SHA-256', 'SHA-384', 'SHA-512', 'HKDF', 'PBKDF2', 'X25519', 'Ed25519', 'X448', 'Ed448'];
const OPS = {
    encrypt: ['RSA-OAEP', 'AES-CTR', 'AES-CBC', 'AES-GCM'],
    decrypt: ['RSA-OAEP', 'AES-CTR', 'AES-CBC', 'AES-GCM'],
    sign: ['RSASSA-PKCS1-v1_5', 'RSA-PSS', 'ECDSA', 'HMAC'],
    verify: ['RSASSA-PKCS1-v1_5', 'RSA-PSS', 'ECDSA', 'HMAC'],
    digest: ['SHA-1', 'SHA-256', 'SHA-384', 'SHA-512'],
    generateKey: ['RSASSA-PKCS1-v1_5', 'RSA-PSS', 'RSA-OAEP', 'ECDSA', 'ECDH', 'AES-CTR', 'AES-CBC', 'AES-GCM', 'AES-KW', 'HMAC', 'X25519'],
    deriveBits: ['ECDH', 'X25519', 'HKDF', 'PBKDF2'],
    importKey: ['RSASSA-PKCS1-v1_5', 'RSA-PSS', 'RSA-OAEP', 'ECDSA', 'ECDH', 'AES-CTR', 'AES-CBC', 'AES-GCM', 'AES-KW', 'HMAC', 'HKDF', 'PBKDF2', 'X25519'],
    wrapKey: ['AES-KW'],
    unwrapKey: ['AES-KW'],
    keyLength: ['AES-CTR', 'AES-CBC', 'AES-GCM', 'AES-KW', 'HMAC', 'HKDF', 'PBKDF2'],
};
const isRsa = (n) => n === 'RSASSA-PKCS1-v1_5' || n === 'RSA-PSS' || n === 'RSA-OAEP';
const isAes = (n) => n.startsWith('AES-');
const isEc = (n) => n === 'ECDSA' || n === 'ECDH';

/* A dictionary member that the specification requires. */
function need(o, k) {
    const v = o[k];
    if (v === undefined) throw new TypeError('The algorithm has no ' + k + ' member');
    return v;
}
/* WebIDL [EnforceRange] unsigned integers */
function uint(v, max, what) {
    const n = Number(v);
    if (!Number.isFinite(n)) throw new TypeError(what + ' is not a finite number');
    const t = Math.trunc(n);
    if (t < 0 || t > max) throw new TypeError(what + ' is out of range');
    return t;
}
/* Normalizes an algorithm for an operation (the specification's
 * "normalize an algorithm"): { name, ...members read once }. */
function normalize(alg, op) {
    let o;
    if (typeof alg === 'string') o = { name: alg };
    else if (alg !== null && typeof alg === 'object') o = alg;
    else throw new TypeError('The algorithm is not a string or an object');
    if (o.name === undefined) throw new TypeError('The algorithm has no name');
    const given = String(o.name), up = given.toUpperCase();
    const name = NAMES.find((n) => n.toUpperCase() === up);
    if (!name || !OPS[op].includes(name)) notSupported('The algorithm ' + given + ' is not supported for ' + op);
    const r = { name };
    const hash = () => normalize(need(o, 'hash'), 'digest').name;
    switch (op === 'keyLength' ? 'keyLength:' + name : op === 'deriveBits' ? 'deriveBits:' + name : name) {
    case 'RSASSA-PKCS1-v1_5': case 'RSA-OAEP': case 'RSA-PSS':
        if (op === 'generateKey') {
            r.modulusLength = uint(need(o, 'modulusLength'), 0xffffffff, 'modulusLength');
            const e = need(o, 'publicExponent');
            if (!(e instanceof Uint8Array) && tagOf(e) !== 'Uint8Array') throw new TypeError('publicExponent is not a Uint8Array');
            r.publicExponent = new Uint8Array(e);
        }
        if (op === 'generateKey' || op === 'importKey') r.hash = hash();
        if (name === 'RSA-PSS' && (op === 'sign' || op === 'verify')) r.saltLength = uint(need(o, 'saltLength'), 0xffffffff, 'saltLength');
        if (name === 'RSA-OAEP' && (op === 'encrypt' || op === 'decrypt' || op === 'wrapKey' || op === 'unwrapKey')) r.label = optBytes(o.label, 'label');
        break;
    case 'ECDSA': case 'ECDH':
        if (op === 'generateKey' || op === 'importKey') r.namedCurve = String(need(o, 'namedCurve'));
        if (name === 'ECDSA' && (op === 'sign' || op === 'verify')) r.hash = hash();
        break;
    case 'deriveBits:ECDH': case 'deriveBits:X25519': {
        const pub = need(o, 'public');
        if (!slots.has(pub)) throw new TypeError('public is not a CryptoKey');
        r.public = pub;
        break;
    }
    case 'deriveBits:HKDF':
        r.hash = hash();
        r.salt = bytes(need(o, 'salt'), 'salt');
        r.info = bytes(need(o, 'info'), 'info');
        break;
    case 'deriveBits:PBKDF2':
        r.hash = hash();
        r.salt = bytes(need(o, 'salt'), 'salt');
        r.iterations = uint(need(o, 'iterations'), 0xffffffff, 'iterations');
        break;
    case 'AES-CTR':
        if (op === 'encrypt' || op === 'decrypt' || op === 'wrapKey' || op === 'unwrapKey') {
            r.counter = bytes(need(o, 'counter'), 'counter');
            r.length = uint(need(o, 'length'), 255, 'length');
        }
        if (op === 'generateKey') r.length = uint(need(o, 'length'), 65535, 'length');
        break;
    case 'AES-CBC':
        if (op === 'encrypt' || op === 'decrypt' || op === 'wrapKey' || op === 'unwrapKey') r.iv = bytes(need(o, 'iv'), 'iv');
        if (op === 'generateKey') r.length = uint(need(o, 'length'), 65535, 'length');
        break;
    case 'AES-GCM':
        if (op === 'encrypt' || op === 'decrypt' || op === 'wrapKey' || op === 'unwrapKey') {
            r.iv = bytes(need(o, 'iv'), 'iv');
            r.additionalData = optBytes(o.additionalData, 'additionalData');
            r.tagLength = o.tagLength === undefined ? 128 : uint(o.tagLength, 255, 'tagLength');
        }
        if (op === 'generateKey') r.length = uint(need(o, 'length'), 65535, 'length');
        break;
    case 'AES-KW':
        if (op === 'generateKey') r.length = uint(need(o, 'length'), 65535, 'length');
        break;
    case 'keyLength:AES-CTR': case 'keyLength:AES-CBC': case 'keyLength:AES-GCM': case 'keyLength:AES-KW':
        r.length = uint(need(o, 'length'), 65535, 'length');
        break;
    case 'HMAC': case 'keyLength:HMAC':
        if (op === 'generateKey' || op === 'importKey' || op === 'keyLength') {
            r.hash = hash();
            if (o.length !== undefined) r.length = uint(o.length, 0xffffffff, 'length');
        }
        break;
    }
    return r;
}
const BLOCK = { 'SHA-1': 512, 'SHA-256': 512, 'SHA-384': 1024, 'SHA-512': 1024 };
const HASH_BITS = { 'SHA-1': 160, 'SHA-256': 256, 'SHA-384': 384, 'SHA-512': 512 };

/* ---------------- CryptoKey ---------------- */

const USAGES = ['encrypt', 'decrypt', 'sign', 'verify', 'deriveKey', 'deriveBits', 'wrapKey', 'unwrapKey'];
const slots = new WeakMap();
const keyOf = (k, what) => {
    const s = k !== null && typeof k === 'object' ? slots.get(k) : undefined;
    if (!s) throw new TypeError((what || 'The key') + ' is not a CryptoKey');
    return s;
};
class CryptoKey {
    constructor() { throw new TypeError('Illegal constructor'); }
    get type() { return keyOf(this).type; }
    get extractable() { return keyOf(this).extractable; }
    get algorithm() { return keyOf(this).algorithm; }
    get usages() { return keyOf(this).usagesObj; }
}
Object.defineProperty(CryptoKey.prototype, Symbol.toStringTag, { value: 'CryptoKey', configurable: true });

/* The usages script asked for: each a KeyUsage (TypeError otherwise), in
 * the specification's order, once each. */
function usageList(u) {
    if (u === null || typeof u !== 'object' || typeof u[Symbol.iterator] !== 'function') throw new TypeError('The usages are not a sequence');
    const set = new Set();
    for (const x of u) {
        const s = String(x);
        if (!USAGES.includes(s)) throw new TypeError(s + ' is not a key usage');
        set.add(s);
    }
    return USAGES.filter((x) => set.has(x));
}
function checkUsages(usages, allowed) {
    for (const u of usages) if (!allowed.includes(u)) syntaxErr('The usage ' + u + ' is not allowed for this key');
}
/* s: { type, extractable, algorithm, usages, h, name, ...extra } */
function makeKey(s) {
    if ((s.type === 'secret' || s.type === 'private') && s.usages.length === 0) syntaxErr('A secret or private key must have usages');
    const k = Object.create(CryptoKey.prototype);
    s.usagesObj = s.usages.slice();
    slots.set(k, s);
    return k;
}
/* The key must be of this algorithm and allow this usage. */
function keyFor(key, name, usage) {
    const s = keyOf(key);
    if (s.name !== name) accessErr('The key is not a ' + name + ' key');
    if (!s.usages.includes(usage)) accessErr('The key may not be used to ' + usage);
    return s;
}

/* What each algorithm's keys may be used for. */
const PAIR_USAGES = {
    'RSASSA-PKCS1-v1_5': [['verify'], ['sign']],
    'RSA-PSS': [['verify'], ['sign']],
    'ECDSA': [['verify'], ['sign']],
    'RSA-OAEP': [['encrypt', 'wrapKey'], ['decrypt', 'unwrapKey']],
    'ECDH': [[], ['deriveKey', 'deriveBits']],
    'X25519': [[], ['deriveKey', 'deriveBits']],
};
const SECRET_USAGES = {
    'AES-CTR': ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'],
    'AES-CBC': ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'],
    'AES-GCM': ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey'],
    'AES-KW': ['wrapKey', 'unwrapKey'],
    'HMAC': ['sign', 'verify'],
    'HKDF': ['deriveKey', 'deriveBits'],
    'PBKDF2': ['deriveKey', 'deriveBits'],
};
const KIND = { SECRET: 1, RSA: 2, EC: 3, X25519: 4 };
const NALG = { 'RSASSA-PKCS1-v1_5': 1, 'RSA-PSS': 2, 'RSA-OAEP': 3, 'ECDSA': 4, 'ECDH': 5, 'X25519': 5 };
const AESMODE = { 'AES-GCM': 1, 'AES-CBC': 2, 'AES-CTR': 3, 'AES-KW': 4 };

/* ---------------- building keys ---------------- */

function secretKey(name, raw, a, extractable, usages) {
    const s = { type: 'secret', extractable, name, usages };
    if (isAes(name)) {
        if (raw.length !== 16 && raw.length !== 24 && raw.length !== 32) dataErr('An AES key is 128, 192 or 256 bits');
        s.algorithm = { name, length: raw.length * 8 };
    } else if (name === 'HMAC') {
        if (!raw.length) dataErr('The HMAC key is empty');
        let len = raw.length * 8;
        if (a.length !== undefined) {
            if (a.length > len || a.length <= len - 8) dataErr('The HMAC key length does not match the data');
            if (a.length !== len) notSupported('HMAC keys whose length is not a whole number of bytes are not supported');
            len = a.length;
        }
        s.hash = a.hash;
        s.algorithm = { name, hash: { name: a.hash }, length: len };
    } else {
        s.algorithm = { name };
    }
    s.h = N.ckSecret(raw);
    raw.fill(0);
    return makeKey(s);
}

/* An RSA key from PKCS #1 DER. */
function rsaKey(name, hash, der, priv, extractable, usages) {
    const h = N.ckImport(KIND.RSA, priv, 0, NALG[name], hash, der);
    der.fill(0);
    return rsaFromHandle(name, hash, h, priv, extractable, usages);
}
function rsaFromHandle(name, hash, h, priv, extractable, usages) {
    const pub = readRsaPublic(u8(N.ckExport(h, true)));
    return makeKey({
        type: priv ? 'private' : 'public', extractable, name, usages, h, hash,
        algorithm: { name, modulusLength: bitLength(pub.n), publicExponent: new Uint8Array(pub.e), hash: { name: hash } },
    });
}
function ecKey(name, curve, raw, priv, extractable, usages) {
    const c = CURVES[curve];
    const h = N.ckImport(KIND.EC, priv, c.bits, NALG[name], undefined, raw);
    raw.fill(0);
    return makeKey({ type: priv ? 'private' : 'public', extractable, name, usages, h, curve, algorithm: { name, namedCurve: curve } });
}
function xKey(raw, priv, extractable, usages) {
    const h = N.ckImport(KIND.X25519, priv, 255, NALG.X25519, undefined, raw);
    raw.fill(0);
    return makeKey({ type: priv ? 'private' : 'public', extractable, name: 'X25519', usages, h, algorithm: { name: 'X25519' } });
}

/* ---------------- importKey ---------------- */

const JWK_KTY = (name) => (isRsa(name) ? 'RSA' : isEc(name) ? 'EC' : name === 'X25519' ? 'OKP' : 'oct');
const RSA_JWK_ALG = {
    'RSASSA-PKCS1-v1_5': { 'SHA-1': 'RS1', 'SHA-256': 'RS256', 'SHA-384': 'RS384', 'SHA-512': 'RS512' },
    'RSA-PSS': { 'SHA-1': 'PS1', 'SHA-256': 'PS256', 'SHA-384': 'PS384', 'SHA-512': 'PS512' },
    'RSA-OAEP': { 'SHA-1': 'RSA-OAEP', 'SHA-256': 'RSA-OAEP-256', 'SHA-384': 'RSA-OAEP-384', 'SHA-512': 'RSA-OAEP-512' },
};
const HMAC_JWK_ALG = { 'SHA-1': 'HS1', 'SHA-256': 'HS256', 'SHA-384': 'HS384', 'SHA-512': 'HS512' };
const aesJwkAlg = (name, bits) => 'A' + bits + name.slice(4);
const jwkUse = (name) => (name === 'HMAC' || name === 'ECDSA' || name === 'RSASSA-PKCS1-v1_5' || name === 'RSA-PSS' ? 'sig' : 'enc');

/* The checks every JWK import makes (kty, use, key_ops, ext). */
function jwkCommon(j, name, extractable, usages) {
    if (j === null || typeof j !== 'object') throw new TypeError('The JWK is not an object');
    if (j.kty !== JWK_KTY(name)) dataErr('The JWK kty is not ' + JWK_KTY(name));
    if (usages.length && j.use !== undefined && j.use !== jwkUse(name)) dataErr('The JWK use does not allow these usages');
    if (j.key_ops !== undefined) {
        if (!Array.isArray(j.key_ops)) dataErr('The JWK key_ops is not an array');
        const ops = j.key_ops.map(String);
        if (new Set(ops).size !== ops.length) dataErr('The JWK key_ops has a duplicate');
        for (const u of usages) if (!ops.includes(u)) dataErr('The JWK key_ops does not allow ' + u);
    }
    if (j.ext === false && extractable) dataErr('The JWK is not extractable');
}

function importKey(format, keyData, alg, extractable, usagesIn) {
    format = String(format);
    if (!['raw', 'spki', 'pkcs8', 'jwk'].includes(format)) throw new TypeError(format + ' is not a key format');
    const a = normalize(alg, 'importKey');
    const name = a.name;
    extractable = !!extractable;
    const usages = usageList(usagesIn);
    let data, jwk;
    if (format === 'jwk') {
        if (keyData === null || typeof keyData !== 'object' || ArrayBuffer.isView(keyData) || keyData instanceof ArrayBuffer)
            throw new TypeError('The JWK is not an object');
        jwk = keyData;
    } else {
        data = bytes(keyData, 'The key data');
    }
    /* secret keys */
    if (SECRET_USAGES[name]) {
        checkUsages(usages, SECRET_USAGES[name]);
        if (name === 'HKDF' || name === 'PBKDF2') {
            if (format !== 'raw') notSupported(name + ' keys are imported in the raw format only');
            if (extractable) syntaxErr(name + ' keys cannot be extractable');
            return secretKey(name, data, a, false, usages);
        }
        if (format === 'raw') return secretKey(name, data, a, extractable, usages);
        if (format !== 'jwk') notSupported(name + ' keys cannot be imported as ' + format);
        jwkCommon(jwk, name, extractable, usages);
        const k = unb64u(jwk.k, 'k');
        if (name === 'HMAC') {
            if (jwk.alg !== undefined && jwk.alg !== HMAC_JWK_ALG[a.hash]) dataErr('The JWK alg does not match the hash');
        } else if (jwk.alg !== undefined && jwk.alg !== aesJwkAlg(name, k.length * 8)) {
            dataErr('The JWK alg does not match the key');
        }
        return secretKey(name, k, a, extractable, usages);
    }
    /* key pairs */
    const [pubUses, privUses] = PAIR_USAGES[name];
    if (isEc(name) && !CURVES[a.namedCurve]) notSupported('The curve ' + a.namedCurve + ' is not supported');
    if (format === 'raw') {
        if (!isEc(name) && name !== 'X25519') notSupported(name + ' keys cannot be imported raw');
        checkUsages(usages, pubUses);
        return name === 'X25519' ? xKey(data, false, extractable, usages) : ecKey(name, a.namedCurve, data, false, extractable, usages);
    }
    if (format === 'spki') {
        checkUsages(usages, pubUses);
        const s = readSpki(data);
        if (isRsa(name)) {
            if (s.oid !== hex(OID.rsa)) dataErr('The key is not an RSA key');
            if (s.params && (s.params.tag !== 5 || s.params.end !== s.params.start)) dataErr('The RSA key has parameters');
            return rsaKey(name, a.hash, new Uint8Array(s.key), false, extractable, usages);
        }
        if (name === 'X25519') {
            if (s.oid !== hex(OID.x25519) || s.params) dataErr('The key is not an X25519 key');
            return xKey(new Uint8Array(s.key), false, extractable, usages);
        }
        if (s.oid !== hex(OID.ec) || !s.params || s.params.tag !== 6) dataErr('The key is not an EC key');
        if (curveOfOid(elemHex(data, s.params)) !== a.namedCurve) dataErr('The key is not on ' + a.namedCurve);
        return ecKey(name, a.namedCurve, new Uint8Array(s.key), false, extractable, usages);
    }
    if (format === 'pkcs8') {
        checkUsages(usages, privUses);
        const p = readPkcs8(data);
        try {
            if (isRsa(name)) {
                if (p.oid !== hex(OID.rsa)) dataErr('The key is not an RSA key');
                if (p.params && (p.params.tag !== 5 || p.params.end !== p.params.start)) dataErr('The RSA key has parameters');
                readRsaPrivate(p.key);
                return rsaKey(name, a.hash, new Uint8Array(p.key), true, extractable, usages);
            }
            if (name === 'X25519') {
                if (p.oid !== hex(OID.x25519) || p.params) dataErr('The key is not an X25519 key');
                const o = derExpect(p.key, 0, p.key.length, 4);
                if (o.end !== p.key.length) dataErr();
                return xKey(new Uint8Array(body(p.key, o)), true, extractable, usages);
            }
            if (p.oid !== hex(OID.ec) || !p.params || p.params.tag !== 6) dataErr('The key is not an EC key');
            if (curveOfOid(elemHex(data, p.params)) !== a.namedCurve) dataErr('The key is not on ' + a.namedCurve);
            return ecPrivate(name, a.namedCurve, p.key, extractable, usages);
        } finally {
            data.fill(0);
        }
    }
    /* jwk */
    jwkCommon(jwk, name, extractable, usages);
    const priv = jwk.d !== undefined;
    checkUsages(usages, priv ? privUses : pubUses);
    if (isRsa(name)) {
        if (jwk.alg !== undefined && jwk.alg !== RSA_JWK_ALG[name][a.hash]) dataErr('The JWK alg does not match the algorithm');
        if (jwk.oth !== undefined) notSupported('Multi-prime RSA keys are not supported');
        const j = {};
        for (const m of priv ? RSA_PRIV : ['n', 'e']) {
            if (jwk[m] === undefined) dataErr('The JWK has no ' + m);
            j[m] = unb64u(jwk[m], m);
            if (!strip(j[m])[0]) dataErr('The JWK member ' + m + ' is zero');
        }
        return rsaKey(name, a.hash, priv ? writeRsaPrivate(j) : writeRsaPublic(j), priv, extractable, usages);
    }
    if (name === 'X25519') {
        if (jwk.crv !== 'X25519') dataErr('The JWK curve is not X25519');
        const x = unb64u(jwk.x, 'x');
        if (!priv) return xKey(x, false, extractable, usages);
        const k = xKey(unb64u(jwk.d, 'd'), true, extractable, usages);
        if (hex(u8(N.ckExport(slots.get(k).h, true))) !== hex(x)) dataErr('The JWK x does not match d');
        return k;
    }
    const c = CURVES[a.namedCurve];
    if (jwk.crv !== a.namedCurve) dataErr('The JWK curve is not ' + a.namedCurve);
    if (name === 'ECDSA' && jwk.alg !== undefined && jwk.alg !== c.jws) dataErr('The JWK alg does not match the curve');
    const x = unb64u(jwk.x, 'x'), y = unb64u(jwk.y, 'y');
    if (x.length !== c.bytes || y.length !== c.bytes) dataErr('The JWK coordinates have the wrong length');
    const point = concat(Uint8Array.of(4), x, y);
    if (!priv) return ecKey(name, a.namedCurve, point, false, extractable, usages);
    const d = unb64u(jwk.d, 'd');
    if (d.length !== c.bytes) dataErr('The JWK d has the wrong length');
    const k = ecKey(name, a.namedCurve, d, true, extractable, usages);
    if (hex(u8(N.ckExport(slots.get(k).h, true))) !== hex(point)) dataErr('The JWK x and y do not match d');
    return k;
}

/* ECPrivateKey (RFC 5915) inside PKCS #8. */
function ecPrivate(name, curve, b, extractable, usages) {
    const c = CURVES[curve];
    const top = der(b, 0, b.length);
    if (top.tag !== 0x30 || top.end !== b.length) dataErr('The key is not an ECPrivateKey');
    const k = derKids(b, top);
    if (k.length < 2 || k.length > 4) dataErr('The key is not an ECPrivateKey');
    const v = derUint(b, k[0]);
    if (v.length !== 1 || v[0] !== 1 || k[1].tag !== 4) dataErr('The key is not an ECPrivateKey');
    let pub = null;
    for (let i = 2; i < k.length; i++) {
        if (k[i].tag === 0xa0) {
            const o = derExpect(b, k[i].start, k[i].end, 6);
            if (o.end !== k[i].end || elemHex(b, o) !== hex(c.oid)) dataErr('The key is not on ' + curve);
        } else if (k[i].tag === 0xa1) {
            const bs = derExpect(b, k[i].start, k[i].end, 3);
            if (bs.end !== k[i].end || bs.end - bs.start < 2 || b[bs.start] !== 0) dataErr('The public key is not valid');
            pub = hex(b.subarray(bs.start + 1, bs.end));
        } else {
            dataErr('The key is not an ECPrivateKey');
        }
    }
    const d = pad(body(b, k[1]), c.bytes);
    const key = ecKey(name, curve, d, true, extractable, usages);
    if (pub !== null && hex(u8(N.ckExport(slots.get(key).h, true))) !== pub) dataErr('The public key does not match the private key');
    return key;
}

/* ---------------- exportKey ---------------- */

const ecAlgId = (curve) => seq(OID.ec, CURVES[curve].oid);
const rsaAlgId = () => seq(OID.rsa, DER_NULL);
const xAlgId = () => seq(OID.x25519);

function exportKey(format, key) {
    format = String(format);
    if (!['raw', 'spki', 'pkcs8', 'jwk'].includes(format)) throw new TypeError(format + ' is not a key format');
    const s = keyOf(key);
    if (!s.extractable) accessErr('The key is not extractable');
    const name = s.name;
    if (format === 'jwk') return exportJwk(s);
    if (s.type === 'secret') {
        if (format !== 'raw' || name === 'HKDF' || name === 'PBKDF2') notSupported(name + ' keys cannot be exported as ' + format);
        return N.ckBytes(s.h);
    }
    if (format === 'raw') {
        if (!isEc(name) && name !== 'X25519') notSupported(name + ' keys cannot be exported raw');
        if (s.type !== 'public') accessErr('Only a public key is exported raw');
        return N.ckExport(s.h, true);
    }
    if (format === 'spki') {
        if (s.type !== 'public') accessErr('Only a public key is exported as spki');
        const pub = u8(N.ckExport(s.h, true));
        const alg = isRsa(name) ? rsaAlgId() : name === 'X25519' ? xAlgId() : ecAlgId(s.curve);
        return writeSpki(alg, pub).buffer;
    }
    if (s.type !== 'private') accessErr('Only a private key is exported as pkcs8');
    let inner;
    if (isRsa(name)) {
        inner = u8(N.ckExport(s.h, false));
        const out = writePkcs8(rsaAlgId(), inner);
        inner.fill(0);
        return out.buffer;
    }
    if (name === 'X25519') {
        inner = octets(u8(N.ckBytes(s.h)));
        const out = writePkcs8(xAlgId(), inner);
        inner.fill(0);
        return out.buffer;
    }
    const d = u8(N.ckExport(s.h, false));
    inner = seq(derInt(Uint8Array.of(1)), octets(d), tlv(0xa1, bitString(u8(N.ckExport(s.h, true)))));
    d.fill(0);
    const out = writePkcs8(ecAlgId(s.curve), inner);
    inner.fill(0);
    return out.buffer;
}

/* The members in the order Chrome gives them (a JsonWebKey dictionary's,
 * which is alphabetical). */
function exportJwk(s) {
    const name = s.name, j = {};
    const put = (o) => { for (const k of Object.keys(o).sort()) if (o[k] !== undefined) j[k] = o[k]; return j; };
    const common = { ext: s.extractable, key_ops: s.usages.slice() };
    if (s.type === 'secret') {
        if (name === 'HKDF' || name === 'PBKDF2') notSupported(name + ' keys cannot be exported as jwk');
        const k = u8(N.ckBytes(s.h));
        const alg = name === 'HMAC' ? HMAC_JWK_ALG[s.hash] : aesJwkAlg(name, k.length * 8);
        const r = put({ ...common, kty: 'oct', alg, k: b64u(k) });
        k.fill(0);
        return r;
    }
    if (isRsa(name)) {
        const alg = RSA_JWK_ALG[name][s.hash];
        if (s.type === 'public') {
            const p = readRsaPublic(u8(N.ckExport(s.h, true)));
            return put({ ...common, kty: 'RSA', alg, n: b64u(p.n), e: b64u(p.e) });
        }
        const der = u8(N.ckExport(s.h, false));
        const p = readRsaPrivate(der);
        const o = { ...common, kty: 'RSA', alg };
        for (const m of RSA_PRIV) o[m] = b64u(p[m]);
        der.fill(0);
        return put(o);
    }
    const pub = u8(N.ckExport(s.h, true));
    if (name === 'X25519') {
        const o = { ...common, kty: 'OKP', crv: 'X25519', x: b64u(pub) };
        if (s.type === 'private') { const d = u8(N.ckBytes(s.h)); o.d = b64u(d); d.fill(0); }
        return put(o);
    }
    const c = CURVES[s.curve];
    const o = { ...common, kty: 'EC', crv: s.curve, x: b64u(pub.subarray(1, 1 + c.bytes)), y: b64u(pub.subarray(1 + c.bytes)) };
    if (s.type === 'private') { const d = u8(N.ckExport(s.h, false)); o.d = b64u(d); d.fill(0); }
    return put(o);
}

/* ---------------- generateKey ---------------- */

function randomBytes(n) {
    const b = N.random(n);
    if (!b) opErr('The random source is not ready');
    return u8(b);
}
/* Runs an RSA generation job to its end, a slice at a time, so the page
 * goes on between slices (natives_crypto.c). */
function finishJob(job) {
    return new Promise((resolve, reject) => {
        const tick = () => {
            let h;
            try { h = N.ckJobStep(job); } catch (e) { reject(conv(e)); return; }
            if (h === undefined) N.setTimeout(tick, 0);
            else resolve(h);
        };
        tick();
    });
}
function generateKey(alg, extractable, usagesIn) {
    const a = normalize(alg, 'generateKey');
    const name = a.name;
    extractable = !!extractable;
    const usages = usageList(usagesIn);
    if (SECRET_USAGES[name]) {
        checkUsages(usages, SECRET_USAGES[name]);
        if (isAes(name)) {
            if (a.length !== 128 && a.length !== 192 && a.length !== 256) opErr('An AES key is 128, 192 or 256 bits');
            return secretKey(name, randomBytes(a.length / 8), {}, extractable, usages);
        }
        let len = a.length === undefined ? BLOCK[a.hash] : a.length;
        if (len === 0) opErr('The HMAC key length is zero');
        if (len % 8) notSupported('HMAC keys whose length is not a whole number of bytes are not supported');
        if (len > 65536 * 8) opErr('The HMAC key is too long');
        return secretKey(name, randomBytes(len / 8), { hash: a.hash }, extractable, usages);
    }
    const [pubUses, privUses] = PAIR_USAGES[name];
    checkUsages(usages, pubUses.concat(privUses));
    const pubU = usages.filter((u) => pubUses.includes(u)), privU = usages.filter((u) => privUses.includes(u));
    if (!privU.length) syntaxErr('The private key would have no usages');
    const pair = (pubKey, privKey) => ({ publicKey: pubKey, privateKey: privKey });
    if (isRsa(name)) {
        const e = strip(a.publicExponent);
        if (e.length > 4) opErr('The public exponent is not supported');
        let ev = 0;
        for (const x of e) ev = ev * 256 + x;
        const job = N.ckGenerate(KIND.RSA, a.modulusLength, ev >>> 0 === ev ? ev : 0, NALG[name], a.hash);
        return finishJob(job).then((h) => {
            const der = u8(N.ckExport(h, true));
            const pubKey = rsaKey(name, a.hash, der, false, true, pubU);
            return pair(pubKey, rsaFromHandle(name, a.hash, h, true, extractable, privU));
        });
    }
    if (name === 'X25519') {
        /* 32 random bytes are an X25519 private key (RFC 7748, section 5);
         * the key keeps them as they are for export, as Chrome does. */
        const privKey = xKey(randomBytes(32), true, extractable, privU);
        return pair(xKey(u8(N.ckExport(slots.get(privKey).h, true)), false, true, pubU), privKey);
    }
    if (!CURVES[a.namedCurve]) notSupported('The curve ' + a.namedCurve + ' is not supported');
    const h = N.ckGenerate(KIND.EC, CURVES[a.namedCurve].bits, 0, NALG[name], undefined);
    const pubKey = ecKey(name, a.namedCurve, u8(N.ckExport(h, true)), false, true, pubU);
    return pair(pubKey, makeKey({ name, curve: a.namedCurve, algorithm: { name, namedCurve: a.namedCurve },
        type: 'private', extractable, usages: privU, h }));
}

/* ---------------- operations ---------------- */

const mustBeKey = (k) => keyOf(k);

function aes(a, s, decrypt, data) {
    const mode = AESMODE[a.name];
    let iv = null, aad = null, param = 0;
    if (a.name === 'AES-CBC') {
        if (a.iv.length !== 16) opErr('The AES-CBC iv must be 16 bytes');
        iv = a.iv;
    } else if (a.name === 'AES-CTR') {
        if (a.counter.length !== 16) opErr('The AES-CTR counter must be 16 bytes');
        if (a.length === 0 || a.length > 128) opErr('The AES-CTR length must be 1 to 128');
        iv = a.counter;
        param = a.length;
    } else if (a.name === 'AES-GCM') {
        if (![32, 64, 96, 104, 112, 120, 128].includes(a.tagLength)) opErr('The AES-GCM tag length is not supported');
        if (a.iv.length === 0) opErr('The AES-GCM iv is empty');
        iv = a.iv;
        aad = a.additionalData || null;
        param = a.tagLength / 8;
    } else if (decrypt ? data.length < 24 || data.length % 8 : data.length < 16 || data.length % 8) {
        opErr('AES-KW data must be a multiple of 8 bytes, at least ' + (decrypt ? 24 : 16));
    }
    return N.ckAes(s.h, mode, decrypt, iv, aad, param, data);
}

function encryptWith(a, s, data, decrypt) {
    if (a.name === 'RSA-OAEP') {
        if (s.type !== (decrypt ? 'private' : 'public')) accessErr('RSA-OAEP ' + (decrypt ? 'decrypts with the private key' : 'encrypts with the public key'));
        return N.ckOaep(s.h, decrypt, s.hash, a.label, data);
    }
    return aes(a, s, decrypt, data);
}

function sign(a, s, data) {
    if (a.name === 'HMAC') return N.ckHmac(s.hash, s.h, data);
    if (s.type !== 'private') accessErr('Signing needs the private key');
    if (a.name === 'RSA-PSS') {
        const std = N.ckPssSalt(s.h, s.hash);
        if (a.saltLength !== std) notSupported('RSA-PSS signs with a salt of ' + std + ' bytes with this key and hash only');
        return N.ckSign(s.h, NALG['RSA-PSS'], s.hash, data);
    }
    return N.ckSign(s.h, NALG[a.name], a.name === 'ECDSA' ? a.hash : s.hash, data);
}

function verify(a, s, sig, data) {
    if (a.name === 'HMAC') {
        const mac = u8(N.ckHmac(s.hash, s.h, data));
        if (mac.length !== sig.length) return false;
        let diff = 0;
        for (let i = 0; i < mac.length; i++) diff |= mac[i] ^ sig[i];
        return diff === 0;
    }
    if (s.type !== 'public') accessErr('Verifying needs the public key');
    if (a.name === 'RSA-PSS') {
        const std = N.ckPssSalt(s.h, s.hash);
        return N.ckVerify(s.h, NALG['RSA-PSS'], s.hash, a.saltLength !== std, data, sig);
    }
    return N.ckVerify(s.h, NALG[a.name], a.name === 'ECDSA' ? a.hash : s.hash, false, data, sig);
}

function deriveBits(a, s, length) {
    if (a.name === 'ECDH' || a.name === 'X25519') {
        const p = keyOf(a.public);
        if (p.type !== 'public') accessErr('The public member is not a public key');
        if (p.name !== s.name) accessErr('The public key is not a ' + s.name + ' key');
        if (p.curve !== s.curve) accessErr('The public key is on another curve');
        if (s.type !== 'private') accessErr('The base key is not a private key');
        const secret = u8(N.ckAgree(s.h, p.h));
        if (length === null || length === undefined) return secret.buffer;
        length = uint(length, 0xffffffff, 'length');
        if (length > secret.length * 8) opErr('The length is longer than the shared secret');
        const n = Math.ceil(length / 8), out = secret.slice(0, n);
        if (length % 8) out[n - 1] &= (0xff << (8 - length % 8)) & 0xff;
        secret.fill(0);
        return out.buffer;
    }
    if (length === null || length === undefined) opErr(a.name + ' needs a length');
    length = uint(length, 0xffffffff, 'length');
    if (length % 8) opErr('The length must be a multiple of 8');
    if (a.name === 'HKDF') {
        if (length > 255 * HASH_BITS[a.hash]) opErr('The length is too long for HKDF with ' + a.hash);
        if (length === 0) return new ArrayBuffer(0);
        return N.ckDerive(2, a.hash, s.h, a.salt, a.info, length / 8);
    }
    if (length === 0) opErr('The length is zero');
    if (a.iterations === 0) opErr('The iteration count is zero');
    if (length > 8 * 1048576) opErr('The length is too long');
    return N.ckDerive(1, a.hash, s.h, a.salt, a.iterations, length / 8);
}

/* The specification's "get key length". */
function keyLength(a) {
    if (isAes(a.name)) {
        if (a.length !== 128 && a.length !== 192 && a.length !== 256) opErr('An AES key is 128, 192 or 256 bits');
        return a.length;
    }
    if (a.name === 'HMAC') {
        if (a.length === undefined) return BLOCK[a.hash];
        if (a.length === 0) throw new TypeError('The HMAC key length is zero');
        return a.length;
    }
    return null;
}

const utf8 = (s) => new Uint8Array(N.utf8Encode(s));

class SubtleCrypto {
    constructor() { throw new TypeError('Illegal constructor'); }
    digest(alg, data) {
        return run(() => {
            const a = normalize(alg, 'digest');
            return N.digest(a.name, bytes(data));
        });
    }
    encrypt(alg, key, data) {
        return run(() => {
            const a = normalize(alg, 'encrypt'), d = bytes(data);
            return encryptWith(a, keyFor(key, a.name, 'encrypt'), d, false);
        });
    }
    decrypt(alg, key, data) {
        return run(() => {
            const a = normalize(alg, 'decrypt'), d = bytes(data);
            return encryptWith(a, keyFor(key, a.name, 'decrypt'), d, true);
        });
    }
    sign(alg, key, data) {
        return run(() => {
            const a = normalize(alg, 'sign'), d = bytes(data);
            return sign(a, keyFor(key, a.name, 'sign'), d);
        });
    }
    verify(alg, key, signature, data) {
        return run(() => {
            const a = normalize(alg, 'verify'), sig = bytes(signature, 'The signature'), d = bytes(data);
            return verify(a, keyFor(key, a.name, 'verify'), sig, d);
        });
    }
    generateKey(alg, extractable, usages) {
        return run(() => generateKey(alg, extractable, usages));
    }
    importKey(format, keyData, alg, extractable, usages) {
        return run(() => importKey(format, keyData, alg, extractable, usages));
    }
    exportKey(format, key) {
        return run(() => exportKey(format, key));
    }
    deriveBits(alg, baseKey, length = null) {
        return run(() => {
            const a = normalize(alg, 'deriveBits');
            return deriveBits(a, keyFor(baseKey, a.name, 'deriveBits'), length);
        });
    }
    deriveKey(alg, baseKey, derivedKeyType, extractable, usages) {
        return run(() => {
            const a = normalize(alg, 'deriveBits');
            const imp = normalize(derivedKeyType, 'importKey');
            const len = keyLength(normalize(derivedKeyType, 'keyLength'));
            const bits = u8(deriveBits(a, keyFor(baseKey, a.name, 'deriveKey'), len));
            return importKey('raw', bits, imp, extractable, usages);
        });
    }
    wrapKey(format, key, wrappingKey, wrapAlgorithm) {
        return run(() => {
            let a;
            try { a = normalize(wrapAlgorithm, 'wrapKey'); } catch (e) {
                if (!(e instanceof G.DOMException) || e.name !== 'NotSupportedError') throw e;
                a = normalize(wrapAlgorithm, 'encrypt');
            }
            const s = keyFor(wrappingKey, a.name, 'wrapKey');
            mustBeKey(key);
            const exported = exportKey(format, key);
            const data = String(format) === 'jwk' ? utf8(JSON.stringify(exported)) : u8(exported);
            try {
                return encryptWith(a, s, data, false);
            } finally {
                data.fill(0);
            }
        });
    }
    unwrapKey(format, wrappedKey, unwrappingKey, unwrapAlgorithm, unwrappedKeyAlgorithm, extractable, usages) {
        return run(() => {
            let a;
            try { a = normalize(unwrapAlgorithm, 'unwrapKey'); } catch (e) {
                if (!(e instanceof G.DOMException) || e.name !== 'NotSupportedError') throw e;
                a = normalize(unwrapAlgorithm, 'decrypt');
            }
            const imp = normalize(unwrappedKeyAlgorithm, 'importKey');
            const wrapped = bytes(wrappedKey, 'The wrapped key');
            const s = keyFor(unwrappingKey, a.name, 'unwrapKey');
            const plain = u8(encryptWith(a, s, wrapped, true));
            let keyData = plain;
            if (String(format) === 'jwk') {
                try { keyData = JSON.parse(N.utf8Decode(plain, true, false)); } catch (e) { dataErr('The unwrapped key is not JSON'); }
                plain.fill(0);
            }
            return importKey(format, keyData, imp, extractable, usages);
        });
    }
}
Object.defineProperty(SubtleCrypto.prototype, Symbol.toStringTag, { value: 'SubtleCrypto', configurable: true });
for (const C of [SubtleCrypto, CryptoKey]) {
    for (const k of Object.getOwnPropertyNames(C.prototype)) {
        if (k === 'constructor') continue;
        const d = Object.getOwnPropertyDescriptor(C.prototype, k);
        d.enumerable = true;
        Object.defineProperty(C.prototype, k, d);
    }
}

return { subtle: Object.create(SubtleCrypto.prototype), SubtleCrypto, CryptoKey };
})
