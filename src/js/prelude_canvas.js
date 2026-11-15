
/* ---------------- canvas ----------------
 * The 2D context's attributes and most of its methods are C functions on
 * N.c2dProto (natives_canvas.c). Here: the interface objects, the canvas
 * element's methods, ImageData, DOMMatrix, OffscreenCanvas,
 * createImageBitmap, and the context methods that build script objects. */

const nativeIface = (name, proto) => {
    const C = { [name]() { throw new TypeError('Illegal constructor'); } }[name];
    Object.defineProperty(C, 'prototype', { value: proto, writable: false });
    Object.defineProperty(proto, 'constructor', { value: C, writable: true, configurable: true });
    Object.defineProperty(proto, Symbol.toStringTag, { value: name, configurable: true });
    defGlobal(name, C);
    return C;
};
nativeIface('CanvasRenderingContext2D', N.c2dProto);
nativeIface('CanvasGradient', N.gradProto);
nativeIface('CanvasPattern', N.patProto);
nativeIface('ImageBitmap', N.bmpProto);
nativeIface('TextMetrics', N.tmProto);
Object.defineProperty(N.Path2D.prototype, Symbol.toStringTag, { value: 'Path2D', configurable: true });
defGlobal('Path2D', N.Path2D);

/* A long argument: a number that is not finite is a TypeError. */
const canvasLong = (v) => {
    v = Number(v);
    if (!Number.isFinite(v)) throw new TypeError('The value is not a finite number');
    return Math.trunc(v) | 0;
};
/* The most pixels an ImageData may hold: 16 MB of data. */
const IMAGE_DATA_MAX = 4194304;

class ImageData {
    constructor(a, b, c) {
        let data, w, h;
        if (a instanceof Uint8ClampedArray) {
            if (arguments.length < 2) throw new TypeError('ImageData: 2 arguments required');
            data = a;
            w = b >>> 0;
            if (!w) throw domErr('The width is zero', 'IndexSizeError');
            if (!data.length || data.length % 4) throw domErr('The data length is not a multiple of 4', 'InvalidStateError');
            const px = data.length / 4;
            if (px % w) throw domErr('The data length is not a multiple of the width', 'IndexSizeError');
            h = px / w;
            if (c !== undefined && (c >>> 0) !== h) throw domErr('The height does not match the data', 'IndexSizeError');
        } else {
            if (arguments.length < 2) throw new TypeError('ImageData: 2 arguments required');
            w = a >>> 0;
            h = b >>> 0;
            if (!w || !h) throw domErr('The width or height is zero', 'IndexSizeError');
            if (w * h > IMAGE_DATA_MAX) throw new RangeError('The ImageData is too large');
            data = new Uint8ClampedArray(w * h * 4);
        }
        Object.defineProperty(this, 'width', { value: w, enumerable: true });
        Object.defineProperty(this, 'height', { value: h, enumerable: true });
        Object.defineProperty(this, 'data', { value: data, enumerable: true });
    }
    get colorSpace() { return 'srgb'; }
}
tag(ImageData);
defGlobal('ImageData', ImageData);

/* DOMMatrix, the two-dimensional part: what getTransform returns and
 * setTransform, addPath and CanvasPattern.setTransform accept. */
class DOMMatrixReadOnly {
    constructor(init) {
        let v = [1, 0, 0, 1, 0, 0];
        if (init !== undefined && init !== null) {
            if (typeof init === 'string') {
                const m = /^\s*matrix\(([^)]*)\)\s*$/.exec(init);
                if (init.trim() !== '' && init.trim() !== 'none') {
                    if (!m) throw domErr('The matrix could not be parsed', 'SyntaxError');
                    v = m[1].split(',').map(Number);
                }
            } else if (typeof init[Symbol.iterator] === 'function') {
                v = Array.from(init, Number);
                if (v.length === 16) v = [v[0], v[1], v[4], v[5], v[12], v[13]];
            }
            if (v.length !== 6) throw new TypeError('A matrix takes 6 or 16 numbers');
        }
        [this.a, this.b, this.c, this.d, this.e, this.f] = v;
    }
    get m11() { return this.a; } get m12() { return this.b; }
    get m21() { return this.c; } get m22() { return this.d; }
    get m41() { return this.e; } get m42() { return this.f; }
    get m13() { return 0; } get m14() { return 0; } get m23() { return 0; } get m24() { return 0; }
    get m31() { return 0; } get m32() { return 0; } get m33() { return 1; } get m34() { return 0; }
    get m43() { return 0; } get m44() { return 1; }
    get is2D() { return true; }
    get isIdentity() { return this.a === 1 && this.b === 0 && this.c === 0 && this.d === 1 && this.e === 0 && this.f === 0; }
    multiply(o) { return new DOMMatrix(this).multiplySelf(o); }
    translate(x = 0, y = 0) { return new DOMMatrix(this).translateSelf(x, y); }
    scale(x = 1, y = x) { return new DOMMatrix(this).scaleSelf(x, y); }
    rotate(deg = 0) { return new DOMMatrix(this).rotateSelf(deg); }
    inverse() { return new DOMMatrix(this).invertSelf(); }
    transformPoint(p = {}) {
        const x = Number(p.x) || 0, y = Number(p.y) || 0;
        return { x: this.a * x + this.c * y + this.e, y: this.b * x + this.d * y + this.f, z: 0, w: 1 };
    }
    toFloat32Array() { return new Float32Array([this.a, this.b, 0, 0, this.c, this.d, 0, 0, 0, 0, 1, 0, this.e, this.f, 0, 1]); }
    toFloat64Array() { return new Float64Array([this.a, this.b, 0, 0, this.c, this.d, 0, 0, 0, 0, 1, 0, this.e, this.f, 0, 1]); }
    toJSON() { return { a: this.a, b: this.b, c: this.c, d: this.d, e: this.e, f: this.f, is2D: true, isIdentity: this.isIdentity }; }
    toString() { return 'matrix(' + [this.a, this.b, this.c, this.d, this.e, this.f].join(', ') + ')'; }
    [Symbol.iterator]() { return [this.a, this.b, this.c, this.d, this.e, this.f][Symbol.iterator](); }
}
class DOMMatrix extends DOMMatrixReadOnly {
    multiplySelf(o) {
        if (!(o instanceof DOMMatrixReadOnly)) {
            const g = (k, k2, d) => (o && o[k] !== undefined ? Number(o[k]) : o && o[k2] !== undefined ? Number(o[k2]) : d);
            o = { a: g('a', 'm11', 1), b: g('b', 'm12', 0), c: g('c', 'm21', 0), d: g('d', 'm22', 1), e: g('e', 'm41', 0), f: g('f', 'm42', 0) };
        }
        const { a, b, c, d, e, f } = this;
        this.a = a * o.a + c * o.b;
        this.b = b * o.a + d * o.b;
        this.c = a * o.c + c * o.d;
        this.d = b * o.c + d * o.d;
        this.e = a * o.e + c * o.f + e;
        this.f = b * o.e + d * o.f + f;
        return this;
    }
    translateSelf(x = 0, y = 0) { return this.multiplySelf({ a: 1, b: 0, c: 0, d: 1, e: Number(x), f: Number(y) }); }
    scaleSelf(x = 1, y = x) { return this.multiplySelf({ a: Number(x), b: 0, c: 0, d: Number(y), e: 0, f: 0 }); }
    rotateSelf(deg = 0) {
        const r = Number(deg) * Math.PI / 180, s = Math.sin(r), k = Math.cos(r);
        return this.multiplySelf({ a: k, b: s, c: -s, d: k, e: 0, f: 0 });
    }
    invertSelf() {
        const { a, b, c, d, e, f } = this;
        const det = a * d - b * c;
        if (!det) { this.a = this.b = this.c = this.d = this.e = this.f = NaN; return this; }
        this.a = d / det;
        this.b = -b / det;
        this.c = -c / det;
        this.d = a / det;
        this.e = (c * f - d * e) / det;
        this.f = (b * e - a * f) / det;
        return this;
    }
}
tag(DOMMatrixReadOnly); tag(DOMMatrix);
defGlobal('DOMMatrixReadOnly', DOMMatrixReadOnly);
defGlobal('DOMMatrix', DOMMatrix);

defProps(N.c2dProto, {
    getTransform() { return new DOMMatrix(N.c2dMatrix(this)); },
    createImageData(a, b) {
        if (a instanceof ImageData) return new ImageData(a.width, a.height);
        if (arguments.length < 2) throw new TypeError('createImageData: 2 arguments required');
        const w = Math.abs(canvasLong(a)), h = Math.abs(canvasLong(b));
        if (!w || !h) throw domErr('The width or height is zero', 'IndexSizeError');
        return new ImageData(w, h);
    },
    getImageData(sx, sy, sw, sh) {
        if (arguments.length < 4) throw new TypeError('getImageData: 4 arguments required');
        sx = canvasLong(sx); sy = canvasLong(sy); sw = canvasLong(sw); sh = canvasLong(sh);
        if (!sw || !sh) throw domErr('The width or height is zero', 'IndexSizeError');
        if (N.canvasTainted(this)) throw domErr('The canvas has been tainted by cross-origin data', 'SecurityError');
        if (sw < 0) { sx += sw; sw = -sw; }
        if (sh < 0) { sy += sh; sh = -sh; }
        const d = new ImageData(sw, sh);
        N.c2dRead(this, sx, sy, sw, sh, d.data);
        return d;
    },
    putImageData(img, dx, dy, x, y, w, h) {
        if (arguments.length < 3) throw new TypeError('putImageData: 3 arguments required');
        if (!(img instanceof ImageData)) throw new TypeError('putImageData: the first argument is not an ImageData');
        dx = canvasLong(dx); dy = canvasLong(dy);
        if (arguments.length >= 7) {
            x = canvasLong(x); y = canvasLong(y); w = canvasLong(w); h = canvasLong(h);
        } else {
            x = 0; y = 0; w = img.width; h = img.height;
        }
        N.c2dWrite(this, img.data, img.width, img.height, dx, dy, x, y, w, h);
    },
});

/* The context of a canvas (an element, or an OffscreenCanvas with owner
 * null), kept in _c2d: getContext returns the same object each time. */
const canvasContext = (canvas, owner, type, opts) => {
    if (str(type) !== '2d') return null; /* webgl, webgl2, bitmaprenderer, webgpu: none */
    let c = canvas._c2d;
    if (c) return c;
    const alpha = !(opts !== null && typeof opts === 'object' && opts.alpha === false);
    N.canvasSync();
    c = N.canvasContext(owner, canvas.width, canvas.height, alpha, canvas);
    Object.defineProperty(canvas, '_c2d', { value: c, configurable: true });
    return c;
};
/* The canvas as PNG: a data: URL or bytes; null without pixels. A canvas
 * that has no context yet encodes as a blank one of its size. */
const canvasEncode = (canvas, asUrl) => {
    const c = canvas._c2d || (canvas.width && canvas.height ? N.canvasContext(null, canvas.width, canvas.height, true, canvas) : null);
    return c ? N.canvasEncode(c, asUrl) : null;
};
const pngBlob = (bytes) => {
    const b = new Blob([], { type: 'image/png' });
    b._b = bytes;
    return b;
};

defProps(HTMLCanvasElement.prototype, {
    getContext(type, opts) { return canvasContext(this, this, type, opts); },
    /* Always PNG: the standard allows it for types an implementation
     * does not encode (JPEG and WebP here). */
    toDataURL() { return canvasEncode(this, true) || 'data:,'; },
    toBlob(cb) {
        if (typeof cb !== 'function') throw new TypeError('toBlob: the callback is not a function');
        const bytes = canvasEncode(this, false);
        N.queueTask(() => cb(bytes ? pngBlob(bytes) : null));
    },
});
/* Setting width or height resets the bitmap, also when the value is the
 * one it has. The attribute is written only when it changes, so that a
 * script clearing its canvas with canvas.width = canvas.width costs no
 * layout. */
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

class OffscreenCanvas {
    constructor(w, h) {
        if (arguments.length < 2) throw new TypeError('OffscreenCanvas: 2 arguments required');
        Object.defineProperty(this, '_ow', { value: w >>> 0, writable: true });
        Object.defineProperty(this, '_oh', { value: h >>> 0, writable: true });
    }
    get width() { return this._ow; }
    set width(v) { this._ow = v >>> 0; if (this._c2d) N.canvasResize(this._c2d, this._ow, this._oh); }
    get height() { return this._oh; }
    set height(v) { this._oh = v >>> 0; if (this._c2d) N.canvasResize(this._c2d, this._ow, this._oh); }
    getContext(type, opts) { return canvasContext(this, null, type, opts); }
    convertToBlob() {
        return new Promise((resolve, reject) => {
            try {
                const bytes = canvasEncode(this, false);
                if (!bytes) throw domErr('The canvas has no pixels', 'IndexSizeError');
                resolve(pngBlob(bytes));
            } catch (e) { reject(e); }
        });
    }
    transferToImageBitmap() {
        const b = N.canvasBitmap(this, false);
        if (this._c2d) N.canvasResize(this._c2d, this._ow, this._oh);
        return b;
    }
}
tag(OffscreenCanvas);
defGlobal('OffscreenCanvas', OffscreenCanvas);

defGlobal('createImageBitmap', function createImageBitmap(source, sx, sy, sw, sh) {
    const crop = arguments.length >= 5;
    return new Promise((resolve, reject) => {
        try {
            if (source instanceof Blob) source = source._b;
            resolve(N.canvasBitmap(source, crop, sx, sy, sw, sh));
        } catch (e) { reject(e); }
    });
});
