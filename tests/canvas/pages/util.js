// Helpers for the canvas pages, loaded after tests/js/harness.js.
function mk(w, h, opts) {
    const c = document.createElement('canvas');
    c.width = w;
    c.height = h;
    return c.getContext('2d', opts);
}
function px(ctx, x, y) { return Array.from(ctx.getImageData(x, y, 1, 1).data); }
// Each channel within tol.
function near(got, want, tol, msg) {
    for (let i = 0; i < want.length; i++)
        if (Math.abs(got[i] - want[i]) > tol)
            throw new Error((msg ? msg + ': ' : '') + 'expected [' + want.join(', ') + '] within ' + tol + ', got [' + Array.from(got).join(', ') + ']');
}
function isPx(ctx, x, y, want, tol, msg) { near(px(ctx, x, y), want, tol === undefined ? 0 : tol, (msg || '') + ' at ' + x + ',' + y); }
const RED = [255, 0, 0, 255], GREEN = [0, 128, 0, 255], BLUE = [0, 0, 255, 255], CLEAR = [0, 0, 0, 0], BLACK = [0, 0, 0, 255], WHITE = [255, 255, 255, 255];
function loadImage(src) {
    return new Promise((resolve, reject) => {
        const img = new Image();
        img.onload = () => resolve(img);
        img.onerror = () => reject(new Error('image failed: ' + src));
        img.src = src;
    });
}
// A 4 x 4 PNG: left half red, right half blue (made with a canvas).
function checkerUrl() {
    const c = mk(4, 4);
    c.fillStyle = '#f00';
    c.fillRect(0, 0, 2, 4);
    c.fillStyle = '#00f';
    c.fillRect(2, 0, 2, 4);
    return c.canvas.toDataURL();
}
