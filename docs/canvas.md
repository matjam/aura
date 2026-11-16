# Canvas

This document covers the `<canvas>` element and its 2D context: the
drawing model, the fast paths, the bindings, tainting, memory and the
bounds on hostile input. Read it before changing `src/paint/canvas2d*.c`,
`src/js/natives_canvas.c` or `src/js/canvas.js`.

The `<canvas>` element has a 2D context (CanvasRenderingContext2D of
the HTML standard). The context is a state machine in C that draws on
an `AuSurface`; script reaches it through C functions on the context's
prototype; layout paints the surface as a replaced box.

## To try it

Load `tests/page/samples/canvas.html`: a bar chart, gradients, a
pattern, paths, compositing, text, an image filtered through
`getImageData`, a CSS-scaled canvas and an animation (Run plays it and
shows the frame time).

## Where things are

| File | Holds |
|---|---|
| `src/paint/canvas2d.h` | the interface; states the limits |
| `canvas2d.c` | sets, bitmaps, canvases, state and pixel access |
| `canvas2d_path.c` | builds paths |
| `canvas2d_draw.c` | paints, compositing, clipping, shadows, fills, strokes, text and images |
| `src/js/natives_canvas.c` | binds them (with `canvas_css.c` for colours and fonts) |
| `src/js/canvas.js` | the element's methods, `ImageData`, `DOMMatrix`, `OffscreenCanvas` and `createImageBitmap`; read the first time script touches a canvas global or calls `getContext`, `toDataURL` or `toBlob` ([`javascript.md`](javascript.md), "Modules read on first use") |
| `src/js/prelude_lazy.js` | the getters that read `canvas.js`, and the element's `width` and `height` setters |
| `src/css/canvasval.c` | parses a colour and a font shorthand given as text (`au_css_parse_color_text`, `au_css_parse_font_text`) with the same code as declarations |
| `src/image/pngenc.c` | the PNG encoder |

## Drawing model

See the top comment of `canvas2d_draw.c`.

- A shape is a coverage from the rasterizer (`au_raster_path`,
  `au_raster_mask`).
- A paint gives a premultiplied source pixel per destination pixel
  (`paint_fetch`: solid, linear, radial and conic gradients through a
  256-entry table, images and patterns sampled through the inverse
  matrix).
- `comp_run` combines them with the operator (`blend_px`: all 26 of
  Compositing and Blending Level 1). The five operators that change
  pixels the shape does not cover (source-in, source-out,
  destination-in, destination-atop, copy) run over the whole clip
  region.
- The context's path is kept in device space; a stroke is made in user
  space (`stroke_outline`) with `au_path_stroke_style`, so a
  non-uniform transform scales the pen.
- A clip is the clip box, plus one coverage byte per pixel when the
  clip path is not a rectangle on whole pixels.
- Shadows blur the shape's alpha with `au_blur_mask` (the box-shadow
  blur of `border.c`).

## Fast paths

Keep them when changing `comp_run` or the image code:

- source-over with a solid colour goes to `au_blend_solid_span`;
- an axis-aligned rectangle (`draw_dev_rect`) builds no path and
  allocates nothing;
- `clearRect` over whole pixels is a store;
- an unscaled image on whole pixels is copied or blended row by row
  (`blit`), and a reduction by 2 or more uses the page painter's
  averaging scaler;
- pixel runs are copied with `c2d_copy_pixels`, not `memcpy`.

## Pixels

A surface pixel is a native 0xAARRGGBB word, premultiplied; `ImageData`
is R, G, B, A bytes, not premultiplied. `au_c2d_row_to_rgba` and
`au_c2d_row_from_rgba` convert a row, one 32-bit word per pixel; the
word differs by byte order, and `test_byte_order` in
`tests/canvas/test_canvas2d.c` runs both. Every premultiplied value
survives the round trip.

## Bindings

- A context object owns its `AuCanvas2D` and holds its canvas in its
  `canvas` property. The canvas keeps its context in `_c2d`.
- Colours go through `js_canvas_parse_color`: hex colours without the
  CSS parser, the last 8 other strings from a cache.
- Setting `font` to the text it already reads back parses nothing.
- The `fillStyle` getter returns the same gradient or pattern object
  (`au_cgradient_user` holds the script object while it lives).

**Size changes.** The DOM attribute hook runs before the change, so
`js_canvas_attr_changing` only notes the element; `js_canvas_sync`
resizes the bitmap at the next canvas call, at `N.canvasSync()` (the
`width` and `height` setters) and when script returns to the embedder.
The setters write the attribute only when the value changes:
`canvas.width = canvas.width` clears the bitmap without a box tree
build.

## Layout and repaint

A canvas is `LR_CANVAS`: its natural size is the `width` and `height`
attributes (300 x 150 by default), which are not presentational hints,
so CSS `width` alone keeps the ratio. `AuLayoutEnv.canvas` gives the
bitmap, or -1 with script off, when the element shows its fallback
content.

The document view owns an `AuCanvasSet` (`au_dv_canvas_set`);
`au_dv_turn_end` asks it which canvases were drawn to and invalidates
the changed part of each canvas's content box, once per script task.
Drawing causes no style pass, tree build or layout.

## Image sources

`image_source` in `natives_canvas.c` resolves a `drawImage` source:

- an `ImageBitmap`;
- a canvas (through the set, or `_c2d` of an `OffscreenCanvas`);
- an image script loaded, whose bytes the prelude keeps in `_ibytes`
  (4 MB each, 12 MB over the live images) until the first draw decodes
  them at full size into `_ibmp`;
- an image of the document through `AuJsHooks.canvas_image`
  (`au_dv_canvas_image`: the image cache's entry, used at its natural
  size).

## Origin-clean

`js_canvas_image_origin_clean` (`natives_canvas.c`) decides whether an
image may be read back: a `data:` or `blob:` URL, or, for http and
https images, what the embedder answers through
`AuJsHooks.image_origin_clean`. The document view answers with
`au_docview_image_origin_clean`, which is true for an image of the
document's origin and for an `<img crossorigin>` whose response passed
the CORS check (see
[`script-requests-and-cors.md`](script-requests-and-cors.md)); without
the hook the URL decides (same origin as the document).

Drawing an image that fails it taints the canvas, as does drawing a
tainted canvas or bitmap, or choosing a pattern made from one;
`getImageData`, `toDataURL` and `toBlob` then throw SecurityError.
Setting the size makes the canvas clean again.

## Memory

- A canvas is at most 4096 pixels on a side and 16 MB; a larger one is
  inert (calls do nothing, `toDataURL` is `"data:,"`).
- The bitmap is allocated at the first draw with `au_try_alloc`.
- A document's canvases, bitmaps and clip masks share 32 MB
  (`AU_C2D_SET_BUDGET`; each nested document has its own set); over it,
  a canvas draws nothing until memory is released.
- `au_docview_memory_estimate` counts them, and a hidden kept document
  keeps them.

## Bounds on hostile input

- A call with an argument that is not finite does nothing.
- Device coordinates are clamped to 900,000.
- A path keeps 250,000 segments (more are dropped: a path is at most
  about 6 MB).
- A fill needing over 500,000 edges or a stroke walking over 60,000
  points draws nothing.
- Gradients keep 256 stops, dash lists 64 entries, the state stack 4096
  entries, a text call 16 KB.

Keep new loops inside these.

## Tests

`tests/canvas`:

- `test_canvas2d`: the context in C (both byte orders, each operator
  against the specification's formulas, limits, the PNG encoder read
  back by the decoder, text against the font list's width); `--bench`
  prints timings.
- `fuzz_canvas2d ITERATIONS SEED [SECONDS]`: random call sequences with
  hostile numbers; run it under the sanitizer build after a change to
  `canvas2d*.c` or the rasterizer.
- `test_canvas_page`: the document view over made-up http origins
  (painting, one repaint per task, no layout, memory, tainting).
- `pages/*.html` run under `aura-host page`.
- `ref/scenes.html` is compared with `golden/*.png`: to write the
  references again, run `aura-host page` with `AURA_UPDATE_GOLDEN=1`
  and the `--ref ID FILE.png` arguments of
  `tests/canvas/CMakeLists.txt`, then check each picture against the
  description beside its scene. The references were checked by eye
  against the standard, not against another browser.

## Measured

Host (Release, CPU time, 300 x 150 canvas, `test_canvas2d --bench`):

| Call | Time |
|---|---|
| an opaque 100 x 100 `fillRect` | 1.4 us |
| the same with alpha | 4.2 us |
| `clearRect` of the canvas | 5.5 us |
| a 100 x 100 `drawImage` unscaled, opaque | 1.5 us |
| the same with alpha | 6.2 us |
| scaled with smoothing | 89 us |
| rotated | 145 us |
| `getImageData` of the canvas | 50 us |
| `putImageData` of the canvas | 54 us |
| a filled circle | 23 us |
| a 12-character `fillText` | 32 us |
| a full linear gradient | 197 us |
| a full radial gradient | 399 us |

A runtime starts 0.7 ms later and holds 67 KB more.

Guest (Enforcer base, QEMU, 2026-11-15): the sample page loads in
455 ms; its animation (a 460 x 160 canvas: a fill, six radial gradient
discs, a rotated stroked square and text per frame) runs at 27 to 31
frames per second, with 19 ms of script and drawing, 1.1 ms of page
paint and 0.1 ms of screen copy per frame (Develop > Performance). The
sample's 13 canvases hold 2.1 MB.

html5test.co (2026-11-15): 2D Graphics went from 0 to 24 of 25 and
Offscreen from 0 to 2 of 3; the total from 285 to 311. Missing: JPEG,
JPEG-XR and WebP export, hit regions and the bitmap renderer context.

## Not done

See [`known-gaps.md`](known-gaps.md#canvas).
