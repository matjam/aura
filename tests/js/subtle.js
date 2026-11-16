// crypto.subtle (src/js/subtle.js, src/js/natives_crypto.c, src/net/webcrypto.c):
// known answers from the standards, round trips, keys and data made by
// OpenSSL, JWKs in the form Chrome gives them, and wrong usages and bad
// input. Digests and HMAC are in net.js.
'use strict';

const S = crypto.subtle;
const hex = (b) => Array.from(new Uint8Array(b), (x) => x.toString(16).padStart(2, '0')).join('');
const unhex = (s) => new Uint8Array((s.match(/../g) || []).map((h) => parseInt(h, 16)));
const b64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
const b64u = (b) => btoa(String.fromCharCode(...new Uint8Array(b))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const utf8 = (s) => new TextEncoder().encode(s);
async function rejects(p, name, msg) {
    let err = null;
    try { await p; } catch (e) { err = e; }
    if (!err) throw new Error((msg ? msg + ': ' : '') + 'expected ' + name + ', it succeeded');
    const got = err instanceof TypeError ? 'TypeError' : err.name;
    if (got !== name) throw new Error((msg ? msg + ': ' : '') + 'expected ' + name + ', got ' + got + ' (' + err.message + ')');
    if (name !== 'TypeError' && !(err instanceof DOMException)) throw new Error((msg ? msg + ': ' : '') + 'not a DOMException');
}
/* An ECDSA signature from OpenSSL (DER) as r || s. */
function derSig(der, n) {
    let p = 2;
    if (der[1] & 0x80) p = 2 + (der[1] & 0x7f);
    const out = new Uint8Array(2 * n);
    for (let i = 0; i < 2; i++) {
        const len = der[p + 1];
        let v = der.subarray(p + 2, p + 2 + len);
        while (v.length > n && v[0] === 0) v = v.subarray(1);
        out.set(v, (i + 1) * n - v.length);
        p += 2 + len;
    }
    return out;
}

/* Made with OpenSSL 4.1 (genpkey, dgst, pkeyutl): an RSA-2048 key, its
 * PKCS #1 v1.5 and PSS (salt 32) signatures of "aura" with SHA-256, and
 * RSA-OAEP encryptions of "aura secret" (SHA-256; SHA-1 with the label
 * "aura"); P-384 and P-521 keys with their ECDSA signatures of "aura". */
const V = {
    rsaP8: 'MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDSps5AhGzlWRyZro4jb0o92VaC3zuFROJzh4KYVtcmeFk2GLhbBN2KQo19NeqUB5dE2NpuLVIicATGrkawG7iAcreHYdWxuz/jid7OMabMGTZxWxWoe9HreTZJCaDmoy3KkBfy68H7+1kq7aMs3SuF2gBbFTecNaOKJ0PbOJ7Lb+0v1NS3daxDd2qdVaatZprnzyeuPJanJtQfRYeOu8UlffFUCsZOhySY4NhMn6lFggkLrxQXkjYVFbDdC5tYtzlNDw8EDDmfYSNODs8d3s/nMpMmzrFHBwGlmpzhSdWjQvATfBHiIN1+JX/j9ck3FFEi6MOx5VH/PW1yOJfM8t9nAgMBAAECggEAHfLcMP0C1Gqib9EK4UvRxNBRBeNB/ZUJEv7JmOoDxbLnkBLcpwgejSkgH5SaSLh5j667A14LJt1BOJQPdbtDNHPa1vMYZB/omeuAk1Uozyyoel5GSoFsG2vMxX6Kcy/w8zMgTIerOS2HhFxrzhWVuf5//XMbSGix2xLxCUpX/Tkm2KLHdASDVWB/7u83sBIxDhLPO7ienNcm75jzksXzm9w6QYmAWneRq3QLIVgA/G3f8MBEReumFsufODkRnYh6ltWgozzr/ws6mOW4I1KaXkpDKAXTfBfS1UIpcci6rUwOH/kAzlSdJ3AKPspHFpHen75RQrYgtPeRK2lB1rqX6QKBgQD3SIRptcta8z0YMEuZvKro/myzRXbDv+rYG4A2aws2K0tnkqMh/eYY+ftKBFTY+Oab33Lc/zneEXV510P2/PwNABIGbg9geenkBL03E9ozTXlP9wNVhBszNa6BfL7Il+tTH01I5yULcw/22ZQ9mEcFQxlHCmNvPuXTs++9ABNpCQKBgQDaE7ls+gAq/RCluABLIuDypk7CpN1xfbjrk+UZ3e30fVPEIgquvh6sAaZ/efMvrmGi0It6/q93o77SZiQgv9keHWxEf8zCnh3r6NS33jPEgtjVOSLr8IyGRXkotLmGuy1RNDlQbINpMlO77eN+G1IUH78xrHvC0pQDxUduzzlQ7wKBgHhq3eBEYPS3QmcHDamd0HSJgRGDoCQOzyPRsIvPcLQ+PQgAxmQFKgm7of0XQuItws7g7yRjPSDAy747b6NU6sDB6gUrj11hwN4C2Ml7Z2RYM90W4o7AOCal24a+4oD4SeY+dfosZUPAEmQY7DFaeCs9049epbFEpdKD6xjUcp9RAoGAaCl5rOKwh9f+RplfkoPjXCMxhWkH2HiLhgJLMJK/YMMn6ouz/IkUz2WuaD8QeBrbyc/Af4v8tbGBqK9e3EGma+rz1rgQTnkFo7O/Pt+kbF1vQlEbLATmqg+u8lF4ODFZUOsX4SEjl3CRbDcRLR/RkYkkYo6XQpQgXLizCOtz3gMCgYEAkBlh4R9IMO3IKbDcJ9IGaDgvdsDOa1/LOZbf/FgV3K+zno+EmHbtWwrJdyrvBwOmaTLcpysqCEEdahHmMtJsBc/93aFhbXH6BtuqLR94nfEexvc0d8pq+tWkQrm6CSkawGBug6wmeDfAOYuDAUCXlQt/4DGVjBR4eTZyvgqmEn4=',
    rsaSpki: 'MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0qbOQIRs5Vkcma6OI29KPdlWgt87hUTic4eCmFbXJnhZNhi4WwTdikKNfTXqlAeXRNjabi1SInAExq5GsBu4gHK3h2HVsbs/44nezjGmzBk2cVsVqHvR63k2SQmg5qMtypAX8uvB+/tZKu2jLN0rhdoAWxU3nDWjiidD2ziey2/tL9TUt3WsQ3dqnVWmrWaa588nrjyWpybUH0WHjrvFJX3xVArGTockmODYTJ+pRYIJC68UF5I2FRWw3QubWLc5TQ8PBAw5n2EjTg7PHd7P5zKTJs6xRwcBpZqc4UnVo0LwE3wR4iDdfiV/4/XJNxRRIujDseVR/z1tcjiXzPLfZwIDAQAB',
    rsaSig: 'QW5VvfpDUjg27NAnnShIEChVjDCYzuxnq59VqLvvZx9vJZN7xb+M06V0PZTAtrQnX05n3pp1AEgLoHtnJ53Z2R1cd40Y+Z/SqP9FkZR9uTp+vXLGOuDy4WC0M+dX1TvPWq+ORdL612ko9RggWE/y6IycMGPfoxyx46AGAWe/yjqPg15V7YLuNshIpUPETnxz/i+zlHmkg2Qfa/AtkaJ24EIDmFKKTLjmD22lD1vhYiZrBB/KjGjrm4kwERucKggxsHrHLdfhIKiunU/udgknQg1sTtvNpQU5x1aElg5n8j5cgaQusYDuCWKBmWJ7wLA9qkI9ItM2xCt/oJzFrZA4DA==',
    pssSig: 'r1hMD/fPiXzBKbFpDDCiJTf1h0NWIJwS7/r9Vjba8Lix/gEzSP68GW+qay7CJhlFIwS4hUBMR4dsIQ+T/gNNDnn4w+wtBczRryheyfagmjQsUPvBI8ZMnm+gnR3snNhO6IecN7Bbgut9V8BYpZ9Pz5JsRXUTZggaKo48B4PONe1/+w/dBhO4Aa87K9vz1HndUtdVHSJu7arPPTpF5b1xRSZ2KAM5InoTgw1FMkArGf8u7uwNNylFErradjxUSbDv+x2WACbUBJ3EjgZDytTxFUOc7zzqqCE31ziDi82N6++1Ru4SH77JZVfOJaKHAiBkDkA+iYp1nfj5S++6vY1O2w==',
    oaep256: 'R+hp4bBwO9PreYx3aT6UOY50rXld2BkL/u/zXbw+1bQQniAkMvBmeBJtSj4zzc0xtJAJB1PceUZKvQyBoVYLa21A8xM2gyN6kGB5Ue2UlUaXjpsnm4IB1dUd+eBRpCdoRJrYWX8CROsnpkzCYHekVLsJPL6j0d9iGwpePysj65n3edzffRpV71t4xeqr1xxyWZLIZSdZtuqU6CVBZq2feeqeGhEx0T8Ba7hMz/NC/85LfRnPts7mWcvZMefe2lUKmpbQ6S1WAzSS+69tHIptpB3/kRYnksiLSgowFbWiZEHxdHhKT6LrQhr/NAAL7YHu5WnGyFs+C2mWoZ0FYRTNFQ==',
    oaep1Label: 'HTJDG/TQppklRXnGYxdyJUV2Lu1bCOnAdzwM6CXn54Y9N2LYIU07OjTPE3LdOsqHFBZR8o93bPMlmOMiKSGy+xVLw54ZKE8y0A5CHV745XvXGA0ltWOQ9Cbts6/3vFKs1aGwcz2W44HJJV3PojEd+RTt2A6Q65UD5yEwhVvf/3Ylbg/pjqnWN5+PMOYe+PGwhlARRH7gHBR45K8zC2S4y+Q8AWzaQQ6hABINP7tBAJfN7iyMkt4En14SCkK35B9W2/ijSunw6k3NSeV6skZtfLwISVQbJmXYk+9CJDXTEqQ+LjJV7xYKHRVVyH61zHKxDAqQDNahZkC/E9YlpiuaMA==',
    p384P8: 'MIG2AgEAMBAGByqGSM49AgEGBSuBBAAiBIGeMIGbAgEBBDDcQBm7zUZ7Tx6PL9Sz4eDb1SaV8eLz8hQdujeU44l5hsgoKZ+ebfEfMPWDVF5G8XehZANiAAQCIvdWj0H8jPpyNZ2OWo2A9krIug7xoPscHBLtHY1YwkvD0L2ELHO8RDGquryUV0yc2Kmo06EQyXS/+m5PGUtOBNKrhoY/qLmQn7uQ2CIRctunTEsG7IzpGOeUTYKYeQk=',
    p384Spki: 'MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEAiL3Vo9B/Iz6cjWdjlqNgPZKyLoO8aD7HBwS7R2NWMJLw9C9hCxzvEQxqrq8lFdMnNipqNOhEMl0v/puTxlLTgTSq4aGP6i5kJ+7kNgiEXLbp0xLBuyM6RjnlE2CmHkJ',
    p384Sig: 'MGYCMQCt3j3oxQqxTE4s1aZa4btrM1JzBBTl3cqBWaURI3sbs63DpGiORpqBgU3gFFdKHHICMQCo+Vajdb3OxKMt5AGqt8c9d27Q+d8wPuFKx2Ej7R0xwZS636fvWaD0yfrPiR2bOE0=',
    p521P8: 'MIHuAgEAMBAGByqGSM49AgEGBSuBBAAjBIHWMIHTAgEBBEIBX38Ez1oW8WyR4THpobkBUmybcE4hOC4e52Fja54m+7Wl4E0yQgYDg2rY7Aqy2JzVkS5qbmKMJKqtcEQFGzQckEmhgYkDgYYABAGcSiOVrFzcXloU1mI/MmLJ4Lqy0/vtxSoXobWhIKml9ZEPhYiIDMelFsyHoJHebchEiUAn5vq2HBiGXqcCIFfWHgG5HATJPrKq1BoAc7Mr79kcmCGOVnZqUH7YmfgYEQzctoD2No0FnTIoay2/Xqfw61pvbpXrw21vwXlwHU2iJPO0RA==',
    p521Spki: 'MIGbMBAGByqGSM49AgEGBSuBBAAjA4GGAAQBnEojlaxc3F5aFNZiPzJiyeC6stP77cUqF6G1oSCppfWRD4WIiAzHpRbMh6CR3m3IRIlAJ+b6thwYhl6nAiBX1h4BuRwEyT6yqtQaAHOzK+/ZHJghjlZ2alB+2Jn4GBEM3LaA9jaNBZ0yKGstv16n8Otab26V68Ntb8F5cB1NoiTztEQ=',
    p521Sig: 'MIGIAkIBVe2CiI2MxNElWpGVNwKvA9Ztl4twlPZM6fmcg4qSuP9SYQvCzB62UE0s1XRZk48k7Cg6vAhxnmkOupTThDzU9AYCQgFFVy+gJ4YUelkKAFWf+PXnuXgHXtrgyyHpN1veAKqRH062X0M6rJhPJMpQpIxPtIDTJVkQrb521XvGjb9vMgX8Jg==',
};
const AURA = utf8('aura');

test('crypto.subtle exists in a secure context, with its interfaces', () => {
    assert.ok(isSecureContext);
    assert.ok('subtle' in crypto);
    assert.ok(S instanceof SubtleCrypto);
    assert.equal(S, crypto.subtle);
    assert.equal(typeof CryptoKey, 'function');
    assert.equal(Object.prototype.toString.call(S), '[object SubtleCrypto]');
    assert.throws(() => new CryptoKey(), TypeError);
    assert.throws(() => new SubtleCrypto(), TypeError);
});

/* ---------------- AES ---------------- */

const aesKey = (name, k, usages, extractable = true) => S.importKey('raw', unhex(k), name, extractable, usages || ['encrypt', 'decrypt']);

promise_test('AES-GCM: the McGrew and Viega test cases 2 and 4', async () => {
    let k = await aesKey('AES-GCM', '00000000000000000000000000000000');
    let ct = await S.encrypt({ name: 'AES-GCM', iv: new Uint8Array(12) }, k, new Uint8Array(16));
    assert.equal(hex(ct), '0388dace60b6a392f328c2b971b2fe78' + 'ab6e47d42cec13bdf53a67b21257bddf');
    k = await aesKey('AES-GCM', 'feffe9928665731c6d6a8f9467308308');
    const iv = unhex('cafebabefacedbaddecaf888'), aad = unhex('feedfacedeadbeeffeedfacedeadbeefabaddad2');
    const pt = unhex('d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39');
    const want = '42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091';
    ct = await S.encrypt({ name: 'AES-GCM', iv, additionalData: aad }, k, pt);
    assert.equal(hex(ct), want + '5bc94fbc3221a5db94fae95ae7121a47');
    assert.equal(hex(await S.decrypt({ name: 'AES-GCM', iv, additionalData: aad }, k, ct)), hex(pt));
    /* a shorter tag is a prefix of the full one */
    const ct96 = await S.encrypt({ name: 'AES-GCM', iv, additionalData: aad, tagLength: 96 }, k, pt);
    assert.equal(hex(ct96), want + '5bc94fbc3221a5db94fae95a');
    assert.equal(hex(await S.decrypt({ name: 'AES-GCM', iv, additionalData: aad, tagLength: 96 }, k, ct96)), hex(pt));
});

promise_test('AES-GCM: tampered data, tags and lengths fail', async () => {
    const k = await aesKey('AES-GCM', 'feffe9928665731c6d6a8f9467308308');
    const iv = new Uint8Array(12);
    const ct = new Uint8Array(await S.encrypt({ name: 'AES-GCM', iv }, k, utf8('attack at dawn')));
    const bad = ct.slice();
    bad[bad.length - 1] ^= 1;
    await rejects(S.decrypt({ name: 'AES-GCM', iv }, k, bad), 'OperationError', 'a tampered tag');
    bad.set(ct);
    bad[0] ^= 1;
    await rejects(S.decrypt({ name: 'AES-GCM', iv }, k, bad), 'OperationError', 'tampered data');
    await rejects(S.decrypt({ name: 'AES-GCM', iv, additionalData: AURA }, k, ct), 'OperationError', 'other additional data');
    await rejects(S.decrypt({ name: 'AES-GCM', iv }, k, ct.subarray(0, 15)), 'OperationError', 'shorter than the tag');
    await rejects(S.decrypt({ name: 'AES-GCM', iv }, k, new Uint8Array(0)), 'OperationError', 'empty');
    await rejects(S.encrypt({ name: 'AES-GCM', iv, tagLength: 100 }, k, AURA), 'OperationError', 'a tag length that is not allowed');
    await rejects(S.encrypt({ name: 'AES-GCM', iv: new Uint8Array(0) }, k, AURA), 'OperationError', 'an empty iv');
    await rejects(S.encrypt({ name: 'AES-GCM' }, k, AURA), 'TypeError', 'no iv');
    /* an iv of another length works (GHASH of the iv) */
    const odd = await S.encrypt({ name: 'AES-GCM', iv: new Uint8Array(7) }, k, AURA);
    assert.equal(new TextDecoder().decode(await S.decrypt({ name: 'AES-GCM', iv: new Uint8Array(7) }, k, odd)), 'aura');
    /* empty plaintext */
    const e = await S.encrypt({ name: 'AES-GCM', iv }, k, new Uint8Array(0));
    assert.equal(e.byteLength, 16);
    assert.equal((await S.decrypt({ name: 'AES-GCM', iv }, k, e)).byteLength, 0);
});

promise_test('AES-CBC: SP 800-38A F.2.1 with PKCS #7 padding, and bad input', async () => {
    const k = await aesKey('AES-CBC', '2b7e151628aed2a6abf7158809cf4f3c');
    const iv = unhex('000102030405060708090a0b0c0d0e0f');
    const ct = await S.encrypt({ name: 'AES-CBC', iv }, k, unhex('6bc1bee22e409f96e93d7e117393172a'));
    assert.equal(hex(ct), '7649abac8119b246cee98e9b12e9197d8964e0b149c10b7b682e6e39aaeb731c');
    assert.equal(hex(await S.decrypt({ name: 'AES-CBC', iv }, k, ct)), '6bc1bee22e409f96e93d7e117393172a');
    for (const n of [0, 1, 15, 17, 100]) {
        const p = new Uint8Array(n).map((_, i) => i);
        const c = await S.encrypt({ name: 'AES-CBC', iv }, k, p);
        assert.equal(c.byteLength, (Math.floor(n / 16) + 1) * 16);
        assert.equal(hex(await S.decrypt({ name: 'AES-CBC', iv }, k, c)), hex(p));
    }
    await rejects(S.encrypt({ name: 'AES-CBC', iv: new Uint8Array(15) }, k, AURA), 'OperationError', 'a 15-byte iv');
    await rejects(S.decrypt({ name: 'AES-CBC', iv }, k, new Uint8Array(ct).subarray(0, 20)), 'OperationError', 'not whole blocks');
    await rejects(S.decrypt({ name: 'AES-CBC', iv }, k, new Uint8Array(0)), 'OperationError', 'empty');
    const bad = new Uint8Array(ct);
    bad[31] ^= 0x55;
    await rejects(S.decrypt({ name: 'AES-CBC', iv }, k, bad), 'OperationError', 'bad padding');
});

promise_test('AES-CTR: SP 800-38A F.5.1, and the counter length', async () => {
    const k = await aesKey('AES-CTR', '2b7e151628aed2a6abf7158809cf4f3c');
    const counter = unhex('f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff');
    const pt = unhex('6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51');
    const ct = await S.encrypt({ name: 'AES-CTR', counter, length: 64 }, k, pt);
    assert.equal(hex(ct), '874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff');
    assert.equal(hex(await S.decrypt({ name: 'AES-CTR', counter, length: 128 }, k, ct)), hex(pt));
    /* With an 8-bit counter the second block's counter is ...ff00, not
     * f0f1...ff + 1 carried into the rest. */
    const two = await S.encrypt({ name: 'AES-CTR', counter, length: 8 }, k, new Uint8Array(32));
    const c2 = counter.slice();
    c2[15] = 0;
    const b1 = await S.encrypt({ name: 'AES-CTR', counter, length: 128 }, k, new Uint8Array(16));
    const b2 = await S.encrypt({ name: 'AES-CTR', counter: c2, length: 128 }, k, new Uint8Array(16));
    assert.equal(hex(two), hex(b1) + hex(b2));
    /* a counter of 1 bit has two values: a third block would repeat one */
    await rejects(S.encrypt({ name: 'AES-CTR', counter, length: 1 }, k, new Uint8Array(33)), 'OperationError', 'the counter would repeat');
    assert.equal((await S.encrypt({ name: 'AES-CTR', counter, length: 1 }, k, new Uint8Array(32))).byteLength, 32);
    await rejects(S.encrypt({ name: 'AES-CTR', counter, length: 0 }, k, pt), 'OperationError', 'length 0');
    await rejects(S.encrypt({ name: 'AES-CTR', counter, length: 129 }, k, pt), 'OperationError', 'length 129');
    await rejects(S.encrypt({ name: 'AES-CTR', counter: new Uint8Array(8), length: 64 }, k, pt), 'OperationError', 'a short counter');
    assert.equal((await S.encrypt({ name: 'AES-CTR', counter, length: 64 }, k, new Uint8Array(0))).byteLength, 0);
});

promise_test('AES-KW: RFC 3394 4.1 and 4.6, unwrapping, and tampering', async () => {
    let kek = await aesKey('AES-KW', '000102030405060708090A0B0C0D0E0F', ['wrapKey', 'unwrapKey']);
    let key = await aesKey('AES-GCM', '00112233445566778899AABBCCDDEEFF');
    let w = await S.wrapKey('raw', key, kek, 'AES-KW');
    assert.equal(hex(w), '1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5');
    const back = await S.unwrapKey('raw', w, kek, 'AES-KW', 'AES-GCM', true, ['encrypt']);
    assert.equal(hex(await S.exportKey('raw', back)), '00112233445566778899aabbccddeeff');
    kek = await aesKey('AES-KW', '000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F', ['wrapKey', 'unwrapKey']);
    key = await aesKey('AES-CBC', '00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F');
    w = await S.wrapKey('raw', key, kek, { name: 'AES-KW' });
    assert.equal(hex(w), '28c9f404c4b810f4cbccb35cfb87f8263f5786e2d80ed326cbc7f0e71a99f43bfb988b9b7a02dd21');
    const bad = new Uint8Array(w);
    bad[5] ^= 1;
    await rejects(S.unwrapKey('raw', bad, kek, 'AES-KW', 'AES-CBC', true, ['encrypt']), 'OperationError', 'a tampered wrap');
    await rejects(S.unwrapKey('raw', new Uint8Array(w).subarray(0, 20), kek, 'AES-KW', 'AES-CBC', true, ['encrypt']), 'OperationError', 'not a multiple of 8');
    await rejects(S.unwrapKey('raw', new Uint8Array(16), kek, 'AES-KW', 'AES-CBC', true, ['encrypt']), 'OperationError', 'too short');
    /* a JWK's JSON is seldom a multiple of 8 bytes */
    await rejects(S.encrypt('AES-KW', kek, new Uint8Array(16)), 'NotSupportedError', 'AES-KW does not encrypt');
    await rejects(S.importKey('raw', new Uint8Array(16), 'AES-KW', true, ['encrypt']), 'SyntaxError', 'an AES-KW key does not encrypt');
});

promise_test('AES keys: generation, raw and jwk in Chrome\'s form, lengths', async () => {
    for (const len of [128, 192, 256]) {
        const k = await S.generateKey({ name: 'AES-CBC', length: len }, true, ['decrypt', 'encrypt', 'encrypt']);
        assert.equal(k.type, 'secret');
        assert.equal(k.algorithm.length, len);
        assert.equal(k.algorithm, k.algorithm, 'the same algorithm object each time');
        assert.equal(k.usages.join(), 'encrypt,decrypt');
        assert.equal((await S.exportKey('raw', k)).byteLength, len / 8);
    }
    await rejects(S.generateKey({ name: 'AES-GCM', length: 100 }, true, ['encrypt']), 'OperationError');
    await rejects(S.importKey('raw', new Uint8Array(15), 'AES-GCM', true, ['encrypt']), 'DataError', '15 bytes');
    const jwk = { alg: 'A128GCM', ext: true, k: 'AAECAwQFBgcICQoLDA0ODw', key_ops: ['encrypt', 'decrypt'], kty: 'oct' };
    const k = await S.importKey('jwk', jwk, 'AES-GCM', true, ['encrypt', 'decrypt']);
    assert.equal(hex(await S.exportKey('raw', k)), '000102030405060708090a0b0c0d0e0f');
    assert.equal(JSON.stringify(await S.exportKey('jwk', k)), JSON.stringify(jwk));
    await rejects(S.importKey('jwk', { ...jwk, alg: 'A256GCM' }, 'AES-GCM', true, ['encrypt']), 'DataError', 'alg and length disagree');
    await rejects(S.importKey('jwk', { ...jwk, kty: 'RSA' }, 'AES-GCM', true, ['encrypt']), 'DataError', 'kty');
    await rejects(S.importKey('jwk', { ...jwk, k: 'AAEC*wQF' }, 'AES-GCM', true, ['encrypt']), 'DataError', 'bad base64url');
    await rejects(S.importKey('jwk', { ...jwk, key_ops: ['encrypt'] }, 'AES-GCM', true, ['decrypt']), 'DataError', 'key_ops');
    await rejects(S.importKey('jwk', { ...jwk, ext: false }, 'AES-GCM', true, ['encrypt']), 'DataError', 'ext false');
    await rejects(S.importKey('jwk', { ...jwk, use: 'sig' }, 'AES-GCM', true, ['encrypt']), 'DataError', 'use sig');
    await rejects(S.importKey('jwk', new Uint8Array(4), 'AES-GCM', true, ['encrypt']), 'TypeError', 'jwk from bytes');
    const ne = await S.importKey('raw', new Uint8Array(16), 'AES-GCM', false, ['encrypt']);
    await rejects(S.exportKey('raw', ne), 'InvalidAccessError', 'not extractable');
    await rejects(S.importKey('raw', new Uint8Array(16), 'AES-GCM', true, []), 'SyntaxError', 'no usages');
    await rejects(S.importKey('raw', new Uint8Array(16), 'AES-GCM', true, ['sign']), 'SyntaxError', 'a usage AES does not have');
    await rejects(S.importKey('raw', new Uint8Array(16), 'AES-GCM', true, ['fly']), 'TypeError', 'not a usage');
    await rejects(S.importKey('pkcs8', new Uint8Array(16), 'AES-GCM', true, ['encrypt']), 'NotSupportedError', 'pkcs8 AES');
});

promise_test('wrapKey and unwrapKey with jwk and AES-GCM', async () => {
    const kek = await S.generateKey({ name: 'AES-GCM', length: 256 }, false, ['wrapKey', 'unwrapKey']);
    const hmac = await S.importKey('raw', utf8('Jefe'), { name: 'HMAC', hash: 'SHA-256' }, true, ['sign', 'verify']);
    const iv = new Uint8Array(12).fill(9);
    const w = await S.wrapKey('jwk', hmac, kek, { name: 'AES-GCM', iv });
    const k = await S.unwrapKey('jwk', w, kek, { name: 'AES-GCM', iv }, { name: 'HMAC', hash: 'SHA-256' }, true, ['sign']);
    assert.equal(JSON.stringify(await S.exportKey('jwk', k)),
        '{"alg":"HS256","ext":true,"k":"SmVmZQ","key_ops":["sign"],"kty":"oct"}');
    await rejects(S.wrapKey('raw', hmac, await S.importKey('raw', new Uint8Array(16), 'AES-GCM', true, ['encrypt']), { name: 'AES-GCM', iv }),
        'InvalidAccessError', 'a key without wrapKey');
    const ne = await S.importKey('raw', utf8('x'), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
    await rejects(S.wrapKey('raw', ne, kek, { name: 'AES-GCM', iv }), 'InvalidAccessError', 'a key that is not extractable');
});

/* ---------------- HMAC ---------------- */

promise_test('HMAC keys: jwk, generation and lengths', async () => {
    const k = await S.generateKey({ name: 'HMAC', hash: 'SHA-512' }, true, ['sign']);
    assert.equal(k.algorithm.length, 1024);
    assert.equal(k.algorithm.hash.name, 'SHA-512');
    const j = await S.exportKey('jwk', k);
    assert.equal(j.alg, 'HS512');
    assert.equal(Object.keys(j).join(), 'alg,ext,k,key_ops,kty');
    await rejects(S.importKey('raw', new Uint8Array(0), { name: 'HMAC', hash: 'SHA-256' }, true, ['sign']), 'DataError', 'an empty key');
    await rejects(S.importKey('raw', new Uint8Array(4), { name: 'HMAC', hash: 'SHA-256', length: 64 }, true, ['sign']), 'DataError', 'length past the data');
    await rejects(S.importKey('raw', new Uint8Array(4), { name: 'HMAC', hash: 'MD5' }, true, ['sign']), 'NotSupportedError', 'MD5');
    await rejects(S.importKey('raw', new Uint8Array(4), { name: 'HMAC' }, true, ['sign']), 'TypeError', 'no hash');
    await rejects(S.importKey('jwk', { kty: 'oct', k: 'AAAA', alg: 'HS1' }, { name: 'HMAC', hash: 'SHA-256' }, true, ['sign']), 'DataError', 'alg and hash disagree');
});

/* ---------------- PBKDF2, HKDF ---------------- */

const pbkdf2 = async (pw, salt, iterations, hash, bits) =>
    hex(await S.deriveBits({ name: 'PBKDF2', salt: utf8(salt), iterations, hash },
        await S.importKey('raw', utf8(pw), 'PBKDF2', false, ['deriveBits']), bits));

promise_test('PBKDF2: RFC 6070 (HMAC-SHA-1) and RFC 7914 (HMAC-SHA-256)', async () => {
    assert.equal(await pbkdf2('password', 'salt', 1, 'SHA-1', 160), '0c60c80f961f0e71f3a9b524af6012062fe037a6');
    assert.equal(await pbkdf2('password', 'salt', 2, 'SHA-1', 160), 'ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957');
    assert.equal(await pbkdf2('password', 'salt', 4096, 'SHA-1', 160), '4b007901b765489abead49d926f721d065a429c1');
    assert.equal(await pbkdf2('passwordPASSWORDpassword', 'saltSALTsaltSALTsaltSALTsaltSALTsalt', 4096, 'SHA-1', 200),
        '3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038');
    assert.equal(await pbkdf2('pass\0word', 'sa\0lt', 4096, 'SHA-1', 128), '56fa6aa75548099dcc37d7f03425e0c3');
    assert.equal(await pbkdf2('passwd', 'salt', 1, 'SHA-256', 512),
        '55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783');
    assert.equal(await pbkdf2('Password', 'NaCl', 80000, 'SHA-256', 512),
        '4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d');
});

promise_test('PBKDF2: deriveKey, and the lengths and counts it refuses', async () => {
    const pw = await S.importKey('raw', utf8('password'), 'PBKDF2', false, ['deriveKey', 'deriveBits']);
    assert.equal(pw.extractable, false);
    const k = await S.deriveKey({ name: 'PBKDF2', salt: utf8('salt'), iterations: 1, hash: 'SHA-1' }, pw, { name: 'AES-CBC', length: 128 }, true, ['encrypt']);
    assert.equal(hex(await S.exportKey('raw', k)), '0c60c80f961f0e71f3a9b524af601206');
    const h = await S.deriveKey({ name: 'PBKDF2', salt: utf8('salt'), iterations: 1, hash: 'SHA-1' }, pw, { name: 'HMAC', hash: 'SHA-1', length: 160 }, true, ['sign']);
    assert.equal(hex(await S.exportKey('raw', h)), '0c60c80f961f0e71f3a9b524af6012062fe037a6');
    const p = { name: 'PBKDF2', salt: utf8('salt'), iterations: 1, hash: 'SHA-256' };
    await rejects(S.deriveBits(p, pw), 'OperationError', 'no length');
    await rejects(S.deriveBits(p, pw, 0), 'OperationError', 'length 0');
    await rejects(S.deriveBits(p, pw, 12), 'OperationError', 'not a multiple of 8');
    await rejects(S.deriveBits({ ...p, iterations: 0 }, pw, 128), 'OperationError', 'no iterations');
    await rejects(S.deriveBits({ ...p, hash: 'MD5' }, pw, 128), 'NotSupportedError', 'MD5');
    await rejects(S.importKey('raw', utf8('pw'), 'PBKDF2', true, ['deriveBits']), 'SyntaxError', 'extractable');
    await rejects(S.importKey('jwk', { kty: 'oct', k: 'AAAA' }, 'PBKDF2', false, ['deriveBits']), 'NotSupportedError', 'jwk');
    await rejects(S.generateKey('PBKDF2', false, ['deriveBits']), 'NotSupportedError', 'generateKey');
    const only = await S.importKey('raw', utf8('password'), 'PBKDF2', false, ['deriveKey']);
    await rejects(S.deriveBits(p, only, 128), 'InvalidAccessError', 'a key without deriveBits');
});

promise_test('HKDF: RFC 5869 test cases 1, 2 and 3', async () => {
    const run = async (ikm, salt, info, bits) =>
        hex(await S.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt: unhex(salt), info: unhex(info) },
            await S.importKey('raw', unhex(ikm), 'HKDF', false, ['deriveBits']), bits));
    const r = (a, b) => Array.from({ length: b - a }, (_, i) => (a + i).toString(16).padStart(2, '0')).join('');
    assert.equal(await run('0b'.repeat(22), r(0, 13), r(0xf0, 0xfa), 336),
        '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865');
    assert.equal(await run(r(0, 0x50), r(0x60, 0xb0), r(0xb0, 0x100), 656),
        'b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87');
    assert.equal(await run('0b'.repeat(22), '', '', 336),
        '8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8');
    const k = await S.importKey('raw', new Uint8Array(8), 'HKDF', false, ['deriveBits']);
    const p = { name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(0), info: new Uint8Array(0) };
    assert.equal((await S.deriveBits(p, k, 0)).byteLength, 0);
    assert.equal((await S.deriveBits(p, k, 255 * 256)).byteLength, 255 * 32);
    await rejects(S.deriveBits(p, k, 255 * 256 + 8), 'OperationError', 'past 255 blocks');
    await rejects(S.deriveBits(p, k, null), 'OperationError', 'no length');
    await rejects(S.deriveBits({ name: 'HKDF', hash: 'SHA-256', info: new Uint8Array(0) }, k, 8), 'TypeError', 'no salt');
});

/* ---------------- RSA ---------------- */

const rsaImport = (name, hash, usagesPub, usagesPriv) => Promise.all([
    S.importKey('spki', b64(V.rsaSpki), { name, hash }, true, usagesPub),
    S.importKey('pkcs8', b64(V.rsaP8), { name, hash }, true, usagesPriv),
]);

promise_test('RSASSA-PKCS1-v1_5: OpenSSL\'s signature, the key formats, jwk', async () => {
    const [pub, priv] = await rsaImport('RSASSA-PKCS1-v1_5', 'SHA-256', ['verify'], ['sign']);
    assert.equal(pub.type, 'public');
    assert.equal(priv.type, 'private');
    assert.equal(pub.algorithm.modulusLength, 2048);
    assert.equal(hex(pub.algorithm.publicExponent), '010001');
    assert.equal(pub.algorithm.hash.name, 'SHA-256');
    assert.equal(await S.verify('RSASSA-PKCS1-v1_5', pub, b64(V.rsaSig), AURA), true);
    /* PKCS #1 v1.5 signatures are deterministic: the same bytes as OpenSSL's */
    assert.equal(hex(await S.sign('RSASSA-PKCS1-v1_5', priv, AURA)), hex(b64(V.rsaSig)));
    assert.equal(await S.verify('RSASSA-PKCS1-v1_5', pub, b64(V.rsaSig), utf8('aurb')), false);
    const bad = b64(V.rsaSig);
    bad[100] ^= 1;
    assert.equal(await S.verify('RSASSA-PKCS1-v1_5', pub, bad, AURA), false, 'a tampered signature');
    assert.equal(await S.verify('RSASSA-PKCS1-v1_5', pub, bad.subarray(1), AURA), false, 'a short signature');
    assert.equal(hex(await S.exportKey('spki', pub)), hex(b64(V.rsaSpki)));
    assert.equal(hex(await S.exportKey('pkcs8', priv)), hex(b64(V.rsaP8)));
    const jwk = await S.exportKey('jwk', priv);
    assert.equal(Object.keys(jwk).join(), 'alg,d,dp,dq,e,ext,key_ops,kty,n,p,q,qi');
    assert.equal(jwk.alg, 'RS256');
    assert.equal(jwk.e, 'AQAB');
    const again = await S.importKey('jwk', jwk, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['sign']);
    assert.equal(hex(await S.sign('RSASSA-PKCS1-v1_5', again, AURA)), hex(b64(V.rsaSig)));
    const pj = await S.exportKey('jwk', pub);
    assert.equal(JSON.stringify(pj), JSON.stringify({ alg: 'RS256', e: 'AQAB', ext: true, key_ops: ['verify'], kty: 'RSA', n: jwk.n }));
    await rejects(S.importKey('jwk', { ...jwk, alg: 'RS384' }, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['sign']), 'DataError', 'alg and hash disagree');
    const noCrt = { kty: 'RSA', n: jwk.n, e: jwk.e, d: jwk.d };
    await rejects(S.importKey('jwk', noCrt, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['sign']), 'DataError', 'no CRT values');
    await rejects(S.importKey('jwk', { ...jwk, dp: jwk.dq }, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['sign']), 'DataError', 'inconsistent CRT values');
    await rejects(S.sign('RSASSA-PKCS1-v1_5', pub, AURA), 'InvalidAccessError', 'signing with the public key');
    await rejects(S.verify('RSASSA-PKCS1-v1_5', priv, b64(V.rsaSig), AURA), 'InvalidAccessError', 'verifying with the private key');
    await rejects(S.importKey('spki', b64(V.rsaSpki), { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, true, ['sign']), 'SyntaxError', 'a public key that signs');
    await rejects(S.exportKey('raw', pub), 'NotSupportedError', 'raw RSA');
    await rejects(S.exportKey('pkcs8', pub), 'InvalidAccessError', 'pkcs8 of a public key');
});

promise_test('RSA key data: truncated and damaged DER', async () => {
    const alg = { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' };
    const p8 = b64(V.rsaP8), spki = b64(V.rsaSpki);
    for (const n of [0, 1, 2, 10, 26, 100, p8.length - 1])
        await rejects(S.importKey('pkcs8', p8.subarray(0, n), alg, true, ['sign']), 'DataError', 'pkcs8 cut at ' + n);
    for (const n of [0, 3, 20, spki.length - 1])
        await rejects(S.importKey('spki', spki.subarray(0, n), alg, true, ['verify']), 'DataError', 'spki cut at ' + n);
    await rejects(S.importKey('spki', concat(spki, [0]), alg, true, ['verify']), 'DataError', 'trailing bytes');
    const lying = spki.slice();
    lying[3] = 0x30; /* a length past the end */
    await rejects(S.importKey('spki', lying, alg, true, ['verify']), 'DataError', 'a length past the end');
    await rejects(S.importKey('spki', b64(V.p384Spki), alg, true, ['verify']), 'DataError', 'an EC key as RSA');
    await rejects(S.importKey('pkcs8', b64(V.rsaSpki), alg, true, ['sign']), 'DataError', 'spki as pkcs8');
});
function concat(a, b) { const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; }

promise_test('RSA-PSS: OpenSSL\'s signature, round trips, salt lengths', async () => {
    const [pub, priv] = await rsaImport('RSA-PSS', 'SHA-256', ['verify'], ['sign']);
    assert.equal(await S.verify({ name: 'RSA-PSS', saltLength: 32 }, pub, b64(V.pssSig), AURA), true);
    assert.equal(await S.verify({ name: 'RSA-PSS', saltLength: 32 }, pub, b64(V.pssSig), utf8('x')), false);
    const sig = await S.sign({ name: 'RSA-PSS', saltLength: 32 }, priv, AURA);
    assert.equal(await S.verify({ name: 'RSA-PSS', saltLength: 32 }, pub, sig, AURA), true);
    await rejects(S.sign({ name: 'RSA-PSS', saltLength: 20 }, priv, AURA), 'NotSupportedError', 'a salt other than the hash length');
    await rejects(S.sign({ name: 'RSA-PSS' }, priv, AURA), 'TypeError', 'no saltLength');
    assert.equal((await S.exportKey('jwk', pub)).alg, 'PS256');
    await rejects(S.verify({ name: 'RSASSA-PKCS1-v1_5' }, pub, sig, AURA), 'InvalidAccessError', 'another algorithm\'s key');
});

promise_test('RSA-OAEP: OpenSSL\'s ciphertexts, labels, round trips, tampering', async () => {
    const [pub, priv] = await rsaImport('RSA-OAEP', 'SHA-256', ['encrypt', 'wrapKey'], ['decrypt', 'unwrapKey']);
    assert.equal(new TextDecoder().decode(await S.decrypt('RSA-OAEP', priv, b64(V.oaep256))), 'aura secret');
    const [, priv1] = await rsaImport('RSA-OAEP', 'SHA-1', ['encrypt'], ['decrypt']);
    assert.equal(new TextDecoder().decode(await S.decrypt({ name: 'RSA-OAEP', label: AURA }, priv1, b64(V.oaep1Label))), 'aura secret');
    await rejects(S.decrypt({ name: 'RSA-OAEP' }, priv1, b64(V.oaep1Label)), 'OperationError', 'the wrong label');
    const ct = await S.encrypt({ name: 'RSA-OAEP', label: utf8('L') }, pub, AURA);
    assert.equal(ct.byteLength, 256);
    assert.equal(new TextDecoder().decode(await S.decrypt({ name: 'RSA-OAEP', label: utf8('L') }, priv, ct)), 'aura');
    const bad = new Uint8Array(ct);
    bad[7] ^= 1;
    await rejects(S.decrypt({ name: 'RSA-OAEP', label: utf8('L') }, priv, bad), 'OperationError', 'tampered');
    await rejects(S.decrypt('RSA-OAEP', priv, bad.subarray(1)), 'OperationError', 'short');
    await rejects(S.encrypt('RSA-OAEP', pub, new Uint8Array(256 - 2 * 32 - 1)), 'OperationError', 'too long for the key');
    assert.equal((await S.encrypt('RSA-OAEP', pub, new Uint8Array(256 - 2 * 32 - 2))).byteLength, 256);
    await rejects(S.encrypt('RSA-OAEP', priv, AURA), 'InvalidAccessError', 'encrypting with the private key');
    /* wrapping an AES key with RSA-OAEP */
    const aes = await aesKey('AES-GCM', '000102030405060708090a0b0c0d0e0f');
    const w = await S.wrapKey('raw', aes, pub, 'RSA-OAEP');
    const u = await S.unwrapKey('raw', w, priv, 'RSA-OAEP', 'AES-GCM', true, ['encrypt']);
    assert.equal(hex(await S.exportKey('raw', u)), '000102030405060708090a0b0c0d0e0f');
    assert.equal((await S.exportKey('jwk', pub)).alg, 'RSA-OAEP-256');
});

promise_test('RSA generateKey: a 1024-bit key pair, and what it refuses', async () => {
    const e = new Uint8Array([1, 0, 1]);
    const kp = await S.generateKey({ name: 'RSASSA-PKCS1-v1_5', modulusLength: 1024, publicExponent: e, hash: 'SHA-1' }, false, ['sign', 'verify']);
    assert.equal(kp.publicKey.algorithm.modulusLength, 1024);
    assert.equal(kp.publicKey.extractable, true);
    assert.equal(kp.privateKey.extractable, false);
    assert.equal(kp.publicKey.usages.join(), 'verify');
    assert.equal(kp.privateKey.usages.join(), 'sign');
    const sig = await S.sign('RSASSA-PKCS1-v1_5', kp.privateKey, AURA);
    assert.equal(await S.verify('RSASSA-PKCS1-v1_5', kp.publicKey, sig, AURA), true);
    await rejects(S.exportKey('pkcs8', kp.privateKey), 'InvalidAccessError', 'not extractable');
    const p = { name: 'RSA-OAEP', modulusLength: 1024, publicExponent: e, hash: 'SHA-256' };
    await rejects(S.generateKey({ ...p, modulusLength: 512 }, true, ['encrypt', 'decrypt']), 'OperationError', '512 bits');
    await rejects(S.generateKey({ ...p, modulusLength: 1028 }, true, ['encrypt', 'decrypt']), 'OperationError', 'not whole bytes');
    await rejects(S.generateKey({ ...p, publicExponent: new Uint8Array([2]) }, true, ['encrypt', 'decrypt']), 'OperationError', 'an even exponent');
    await rejects(S.generateKey({ ...p, publicExponent: new Uint8Array([1, 0, 0, 0, 0, 1]) }, true, ['encrypt', 'decrypt']), 'OperationError', 'a long exponent');
    await rejects(S.generateKey(p, true, ['encrypt']), 'SyntaxError', 'no usage for the private key');
    await rejects(S.generateKey(p, true, ['sign']), 'SyntaxError', 'a usage RSA-OAEP does not have');
    await rejects(S.generateKey({ ...p, publicExponent: [1, 0, 1] }, true, ['decrypt']), 'TypeError', 'an exponent that is not a Uint8Array');
});

/* ---------------- ECDSA ---------------- */

/* RFC 6979, A.2.5: the P-256 key and its signature of "sample" with SHA-256. */
const P256 = {
    d: 'c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721',
    x: '60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6',
    y: '7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299',
    r: 'efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716',
    s: 'f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8',
};
const ecJwk = (k, priv) => ({ crv: 'P-256', ...(priv ? { d: b64u(unhex(k.d)) } : {}), ext: true, key_ops: [priv ? 'sign' : 'verify'], kty: 'EC', x: b64u(unhex(k.x)), y: b64u(unhex(k.y)) });

promise_test('ECDSA P-256: RFC 6979\'s signature, jwk in Chrome\'s form, raw', async () => {
    const alg = { name: 'ECDSA', namedCurve: 'P-256' }, sha = { name: 'ECDSA', hash: 'SHA-256' };
    const pub = await S.importKey('jwk', ecJwk(P256, false), alg, true, ['verify']);
    assert.equal(await S.verify(sha, pub, unhex(P256.r + P256.s), utf8('sample')), true);
    assert.equal(await S.verify(sha, pub, unhex(P256.r + P256.s), utf8('samplf')), false);
    assert.equal(await S.verify(sha, pub, unhex(P256.r + P256.s).subarray(1), utf8('sample')), false, 'a short signature');
    assert.equal(await S.verify({ name: 'ECDSA', hash: 'SHA-384' }, pub, unhex(P256.r + P256.s), utf8('sample')), false, 'another hash');
    const priv = await S.importKey('jwk', ecJwk(P256, true), alg, true, ['sign']);
    assert.equal(JSON.stringify(await S.exportKey('jwk', priv)), JSON.stringify(ecJwk(P256, true)));
    assert.equal(JSON.stringify(await S.exportKey('jwk', pub)), JSON.stringify(ecJwk(P256, false)));
    const sig = await S.sign(sha, priv, AURA);
    assert.equal(sig.byteLength, 64);
    assert.equal(await S.verify(sha, pub, sig, AURA), true);
    assert.equal(hex(await S.exportKey('raw', pub)), '04' + P256.x + P256.y);
    const raw = await S.importKey('raw', unhex('04' + P256.x + P256.y), alg, true, ['verify']);
    assert.equal(await S.verify(sha, raw, sig, AURA), true);
    /* pkcs8 round trip: Chrome's form has the public key and no parameters */
    const p8 = new Uint8Array(await S.exportKey('pkcs8', priv));
    assert.equal(hex(p8.subarray(0, 36)), '308187020100301306072a8648ce3d020106082a8648ce3d030107046d306b0201010420');
    const again = await S.importKey('pkcs8', p8, alg, true, ['sign']);
    assert.equal(await S.verify(sha, pub, await S.sign(sha, again, AURA), AURA), true);
    await rejects(S.importKey('jwk', { ...ecJwk(P256, true), alg: 'ES384' }, alg, true, ['sign']), 'DataError', 'alg ES384 on P-256');
    await rejects(S.importKey('jwk', { ...ecJwk(P256, true), crv: 'P-384' }, alg, true, ['sign']), 'DataError', 'another curve');
    const wrongY = ecJwk(P256, false);
    wrongY.y = b64u(unhex(P256.y.slice(0, -2) + '00'));
    await rejects(S.importKey('jwk', wrongY, alg, true, ['verify']), 'DataError', 'a point off the curve');
    const wrongD = ecJwk(P256, true);
    wrongD.x = b64u(unhex(P256.y));
    await rejects(S.importKey('jwk', wrongD, alg, true, ['sign']), 'DataError', 'x and y that are not d\'s');
    await rejects(S.importKey('raw', unhex('02' + P256.x), alg, true, ['verify']), 'DataError', 'a compressed point');
    await rejects(S.importKey('raw', unhex('04' + P256.x), alg, true, ['verify']), 'DataError', 'half a point');
    await rejects(S.importKey('raw', unhex('04' + P256.x + P256.y), { name: 'ECDSA', namedCurve: 'P-192' }, true, ['verify']), 'NotSupportedError', 'P-192');
    await rejects(S.exportKey('raw', priv), 'InvalidAccessError', 'raw private key');
    await rejects(S.sign({ name: 'ECDSA' }, priv, AURA), 'TypeError', 'no hash');
});

promise_test('ECDSA P-384 and P-521: OpenSSL\'s keys and signatures', async () => {
    for (const [curve, n, hash, spki, p8, sigDer] of [['P-384', 48, 'SHA-384', V.p384Spki, V.p384P8, V.p384Sig],
        ['P-521', 66, 'SHA-512', V.p521Spki, V.p521P8, V.p521Sig]]) {
        const alg = { name: 'ECDSA', namedCurve: curve }, sha = { name: 'ECDSA', hash };
        const pub = await S.importKey('spki', b64(spki), alg, true, ['verify']);
        const priv = await S.importKey('pkcs8', b64(p8), alg, true, ['sign']);
        const sig = derSig(b64(sigDer), n);
        assert.equal(await S.verify(sha, pub, sig, AURA), true, curve + ' OpenSSL\'s signature');
        const mine = await S.sign(sha, priv, AURA);
        assert.equal(mine.byteLength, 2 * n);
        assert.equal(await S.verify(sha, pub, mine, AURA), true, curve + ' a signature made here');
        assert.equal(hex(await S.exportKey('spki', pub)), hex(b64(spki)), curve + ' spki');
        assert.equal(hex(await S.exportKey('pkcs8', priv)), hex(b64(p8)), curve + ' pkcs8');
        const j = await S.exportKey('jwk', priv);
        assert.equal(j.crv, curve);
        await rejects(S.importKey('spki', b64(spki), { name: 'ECDSA', namedCurve: curve === 'P-384' ? 'P-521' : 'P-384' }, true, ['verify']), 'DataError', curve + ' as another curve');
    }
    for (const curve of ['P-256', 'P-384', 'P-521']) {
        const kp = await S.generateKey({ name: 'ECDSA', namedCurve: curve }, true, ['sign', 'verify']);
        const sig = await S.sign({ name: 'ECDSA', hash: 'SHA-256' }, kp.privateKey, AURA);
        assert.equal(await S.verify({ name: 'ECDSA', hash: 'SHA-256' }, kp.publicKey, sig, AURA), true, curve + ' generated');
        const back = await S.importKey('jwk', await S.exportKey('jwk', kp.privateKey), { name: 'ECDSA', namedCurve: curve }, true, ['sign']);
        assert.equal(back.type, 'private');
    }
});

promise_test('EC key data: truncated pkcs8 and spki', async () => {
    const alg = { name: 'ECDSA', namedCurve: 'P-384' };
    const p8 = b64(V.p384P8), spki = b64(V.p384Spki);
    for (let n = 0; n < p8.length; n += 7)
        await rejects(S.importKey('pkcs8', p8.subarray(0, n), alg, true, ['sign']), 'DataError', 'pkcs8 cut at ' + n);
    for (let n = 0; n < spki.length; n += 5)
        await rejects(S.importKey('spki', spki.subarray(0, n), alg, true, ['verify']), 'DataError', 'spki cut at ' + n);
    /* the public key in an ECPrivateKey must be the private key's */
    const other = p8.slice();
    other[other.length - 1] ^= 1;
    await rejects(S.importKey('pkcs8', other, alg, true, ['sign']), 'DataError', 'a public key that is not the private key\'s');
});

/* ---------------- ECDH, X25519 ---------------- */

promise_test('ECDH P-256: the CAVS vector, truncation, deriveKey, wrong keys', async () => {
    /* NIST CAVS 14.1 ECC CDH, P-256, COUNT = 0 */
    const peer = { x: '700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287', y: 'db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac' };
    const me = { d: '7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534', x: 'ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b230', y: '28af61281fd35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141' };
    const Z = '46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b';
    const alg = { name: 'ECDH', namedCurve: 'P-256' };
    const pub = await S.importKey('raw', unhex('04' + peer.x + peer.y), alg, true, []);
    const priv = await S.importKey('jwk', { kty: 'EC', crv: 'P-256', d: b64u(unhex(me.d)), x: b64u(unhex(me.x)), y: b64u(unhex(me.y)) }, alg, true, ['deriveBits', 'deriveKey']);
    assert.equal(hex(await S.deriveBits({ name: 'ECDH', public: pub }, priv, 256)), Z);
    assert.equal(hex(await S.deriveBits({ name: 'ECDH', public: pub }, priv)), Z, 'no length: all of it');
    assert.equal(hex(await S.deriveBits({ name: 'ECDH', public: pub }, priv, 128)), Z.slice(0, 32));
    assert.equal(hex(await S.deriveBits({ name: 'ECDH', public: pub }, priv, 12)), '46f0', 'a length in bits');
    await rejects(S.deriveBits({ name: 'ECDH', public: pub }, priv, 264), 'OperationError', 'longer than the secret');
    const k = await S.deriveKey({ name: 'ECDH', public: pub }, priv, { name: 'AES-GCM', length: 128 }, true, ['encrypt']);
    assert.equal(hex(await S.exportKey('raw', k)), Z.slice(0, 32));
    const other = await S.generateKey({ name: 'ECDH', namedCurve: 'P-384' }, true, ['deriveBits']);
    await rejects(S.deriveBits({ name: 'ECDH', public: other.publicKey }, priv, 128), 'InvalidAccessError', 'a key on another curve');
    await rejects(S.deriveBits({ name: 'ECDH', public: priv }, priv, 128), 'InvalidAccessError', 'a private key as the public');
    const ecdsa = await S.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign']);
    await rejects(S.deriveBits({ name: 'ECDH', public: ecdsa.publicKey }, priv, 128), 'InvalidAccessError', 'an ECDSA key as the public');
    await rejects(S.deriveBits({ name: 'ECDH' }, priv, 128), 'TypeError', 'no public key');
    await rejects(S.importKey('raw', unhex('04' + peer.x + peer.y), alg, true, ['deriveBits']), 'SyntaxError', 'a public key with usages');
    /* two generated pairs agree */
    const a = await S.generateKey({ name: 'ECDH', namedCurve: 'P-521' }, true, ['deriveBits']);
    const b = await S.generateKey({ name: 'ECDH', namedCurve: 'P-521' }, true, ['deriveBits']);
    const ab = await S.deriveBits({ name: 'ECDH', public: b.publicKey }, a.privateKey, 528);
    assert.equal(hex(ab), hex(await S.deriveBits({ name: 'ECDH', public: a.publicKey }, b.privateKey, 528)));
    assert.equal(ab.byteLength, 66);
});

promise_test('X25519: RFC 7748 6.1 with RFC 8037\'s JWKs, raw, pkcs8', async () => {
    const alice = { d: '77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a', x: '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a' };
    const bob = { d: '5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb', x: 'de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f' };
    const K = '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742';
    /* RFC 8037, A.6 */
    const aliceJwk = { kty: 'OKP', crv: 'X25519', d: 'dwdtCnMYpX08FsFyUbJmRd9ML4frwJkqsXf7pR25LCo', x: 'hSDwCYkwp1R0i33ctD73Wg2_Og0mOBr066SpjqqbTmo' };
    const bobJwk = { kty: 'OKP', crv: 'X25519', x: '3p7bfXt9wbTTW2HC7OQ1Nz-DQ8hbeGdNrfx-FG-IK08' };
    const a = await S.importKey('jwk', aliceJwk, 'X25519', true, ['deriveBits']);
    const B = await S.importKey('jwk', bobJwk, 'X25519', true, []);
    assert.equal(hex(await S.deriveBits({ name: 'X25519', public: B }, a, 256)), K);
    const b = await S.importKey('pkcs8', unhex('302e020100300506032b656e04220420' + bob.d), { name: 'X25519' }, true, ['deriveBits']);
    const A = await S.importKey('raw', unhex(alice.x), 'X25519', true, []);
    assert.equal(hex(await S.deriveBits({ name: 'X25519', public: A }, b, null)), K);
    assert.equal(hex(await S.exportKey('raw', B)), bob.x);
    assert.equal(hex(await S.exportKey('pkcs8', b)), '302e020100300506032b656e04220420' + bob.d);
    assert.equal(hex(await S.exportKey('spki', A)), '302a300506032b656e032100' + alice.x);
    assert.equal(JSON.stringify(await S.exportKey('jwk', a)), JSON.stringify({ crv: 'X25519', d: aliceJwk.d, ext: true, key_ops: ['deriveBits'], kty: 'OKP', x: aliceJwk.x }));
    await rejects(S.importKey('jwk', { ...aliceJwk, x: bobJwk.x }, 'X25519', true, ['deriveBits']), 'DataError', 'x that is not d\'s');
    await rejects(S.importKey('raw', new Uint8Array(32), 'X25519', true, []), 'DataError', 'a low-order point');
    await rejects(S.importKey('raw', new Uint8Array(31), 'X25519', true, []), 'DataError', '31 bytes');
    const kp = await S.generateKey('X25519', true, ['deriveKey', 'deriveBits']);
    const kq = await S.generateKey({ name: 'X25519' }, true, ['deriveBits']);
    assert.equal(hex(await S.deriveBits({ name: 'X25519', public: kq.publicKey }, kp.privateKey, 256)),
        hex(await S.deriveBits({ name: 'X25519', public: kp.publicKey }, kq.privateKey, 256)));
    assert.equal(kp.publicKey.usages.length, 0);
    await rejects(S.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']), 'NotSupportedError', 'Ed25519');
    await rejects(S.importKey('raw', new Uint8Array(32), 'Ed25519', true, ['verify']), 'NotSupportedError', 'Ed25519 import');
});

/* ---------------- general ---------------- */

/* Last, as it leaves the keys' memory to the collector. */
const lastTest = () => promise_test('keys held outside the script heap have a limit', async () => {
    /* 4 MB (AU_WC_HELD_MAX): about 64 HMAC keys of 64 KB each */
    const keys = [];
    let err = null;
    try {
        for (let i = 0; i < 200; i++)
            keys.push(await S.importKey('raw', new Uint8Array(65536), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']));
    } catch (e) { err = e; }
    assert.ok(err instanceof DOMException && err.name === 'OperationError', 'refused: ' + err);
    assert.ok(keys.length >= 50 && keys.length < 200, keys.length + ' keys');
    await rejects(S.importKey('raw', new Uint8Array(65537), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']), 'DataError', 'over 64 KB');
    keys.length = 0;
});

promise_test('algorithm names, CryptoKey objects and bad arguments', async () => {
    await rejects(S.encrypt({ name: 'AES-XTS' }, null, AURA), 'NotSupportedError', 'an unknown algorithm');
    await rejects(S.encrypt({}, null, AURA), 'TypeError', 'no name');
    await rejects(S.encrypt({ name: 'aes-gcm', iv: new Uint8Array(12) }, {}, AURA), 'TypeError', 'not a CryptoKey');
    await rejects(S.digest('SHA-256', 'text'), 'TypeError', 'a string as data');
    const k = await aesKey('aes-ctr', '000102030405060708090a0b0c0d0e0f');
    assert.equal(k.algorithm.name, 'AES-CTR');
    assert.equal(k.usages, k.usages, 'the same usages object each time');
    await rejects(S.encrypt({ name: 'AES-CBC', iv: new Uint8Array(16) }, k, AURA), 'InvalidAccessError', 'a CTR key used for CBC');
    const encOnly = await aesKey('AES-GCM', '000102030405060708090a0b0c0d0e0f', ['encrypt']);
    await rejects(S.decrypt({ name: 'AES-GCM', iv: new Uint8Array(12) }, encOnly, new Uint8Array(32)), 'InvalidAccessError', 'a key without decrypt');
    /* the data is copied when the method is called */
    const data = new Uint8Array(16);
    const p = S.encrypt({ name: 'AES-CTR', counter: new Uint8Array(16), length: 64 }, k, data);
    data.fill(255);
    const z = await S.encrypt({ name: 'AES-CTR', counter: new Uint8Array(16), length: 64 }, k, new Uint8Array(16));
    assert.equal(hex(await p), hex(z));
    assert.equal((await S.digest('SHA-1', new ArrayBuffer(0))).byteLength, 20, 'empty data');
    /* keys are not cloned (not done: docs/known-gaps.md) rather than cloned empty */
    assert.throws(() => structuredClone(k), 'DataCloneError');
});

lastTest();
