# Images

This document covers the image decoders in `src/image`, the
per-document image cache and how the document view uses them. Read it
before changing a decoder, the cache or how images reach layout. SVG
images are in [`svg.md`](svg.md); `srcset`, `<picture>` and
`object-fit` are in [`layout.md`](layout.md).

## The rule for decoders

The decoders in `src/image` parse hostile data on a machine without
memory protection.

- Every length comes from the file and is checked before use.
- Every size computation is checked for overflow.
- Dimensions are capped at 32768 and source pixels at 64 million
  (`AuImageOptions`).
- The decoded canvas never exceeds `max_bytes` (16 MB by default), and
  a larger image decodes at 1/2, 1/4 and so on down to 1/256, or fails.

Keep it that way: a change to a decoder needs a vector, a truncation
test and a fuzz run under the sanitizers.

## To try a file

Run `build/host/src/platform/host/aura-host image FILE -o out.png`. It
prints the format, size, decode time and peak memory.

| Option | Effect |
|---|---|
| `--scale N` | decode reduced |
| `--target WxH` | decode for a displayed size |
| `--chunk N` | feed N bytes at a time |
| `--frames` | decode the animation's frames |
| `--repeat N` | decode N times |
| `--favicon SIZE` | pick and scale an icon |

`tests/page/samples/images.html` shows every format; its images are
drawn by `tests/page/make-sample-images.py`.

## To decode bytes

Create an `AuImageDecoder` with `au_image_decoder_new` and call
`au_image_decode` with each chunk as it arrives (`image.h`).

- The size is known as soon as the header arrives (`au_image_info`),
  rows fill in as they decode (`au_image_take_dirty`), and each
  animation frame returns `AU_IMAGE_FRAME`.
- Output is a premultiplied `AuSurface`; its `opaque` flag lets
  painting copy instead of blend.
- `au_image_decode_buffer` decodes a whole file, `au_image_probe` reads
  only the header, and `au_image_decode_favicon` picks and scales an
  icon.

## Formats

One file per format:

| File | Format |
|---|---|
| `png.c` | all colour types and depths, Adam7, APNG, CRCs checked |
| `jpeg.c` | baseline and progressive Huffman, CMYK and YCCK, restarts, EXIF orientation, reduced decoding by IDCT scaling |
| `gif.c` | GIF |
| `webp.c` with `webp_vp8.c` and `webp_vp8l.c` | lossy, lossless, alpha, animation |
| `bmp.c` | BMP and ICO |
| `svgimg.c` | SVG ([`svg.md`](svg.md)) |
| `pngenc.c` | the PNG encoder, for the canvas ([`canvas.md`](canvas.md)) |

`image.c` holds what they share: sniffing, the limits, row reduction,
orientation and frame composition. Each codec is a state machine that
consumes every byte it is given, so chunk boundaries never matter.

## The cache

The cache (`cache.h`) holds one document's images by URL.

- Layout gets the size from the header; pixels are decoded only when an
  image is used in a paint (`au_image_entry_use`), at the smallest
  power-of-two reduction that still covers the displayed size.
- The budget is the "Image memory per page" preference
  (`content.image_memory_kb`, Preferences > Content), set when a page
  starts (`au_docview_set_image_budget` from `page_doc.c`). Automatic
  (0, the default) is an eighth of the memory free, at least 8 MB and
  at most 64 MB; Custom is 4 to 256 MB (`au_image_memory_limit`,
  `scriptmem.h`). On 64-bit builds Automatic is four times that
  (`AU_IMAGE_BUDGET_SCALE`), so an image up to the 16 MB per-image cap
  decodes at full size. Until 2026-11-18 the budget was the memory
  cache size, 8 MB: Reddit's feed dropped 41 of 50 decodes in the guest
  with 330 MB free, and scrolling back decoded them again.
- Over the budget, images not used in the last paint lose their pixels,
  oldest first, and keep their compressed bytes to decode again.
- One image may use at most half the budget (at least 1 MB, at most
  16 MB). A larger image decodes reduced and is drawn scaled up. It is
  decoded again only when shown larger than the last decode's target,
  not on each paint: macintoshgarden.org's 1200 x 1200 body background
  (5.8 MB) was decoded on every scroll step, about 29 ms each in the
  macOS app (2026-11-16).
- An image decoded larger than it is painted (a prefetch below the
  view decodes before layout gives it a size) is decoded again smaller
  once it has been painted at half its decoded size or less for eight
  paints in a row (`shrink_when_small` in `cache.c`). A paint that shows
  it larger starts the count over. Until then each paint box-filtered
  it down, and it held its full-size pixels.
- A still image whose bytes are larger than its pixels keeps only the
  pixels.
- Animations decode frame by frame from the compressed bytes, keep one
  canvas, and pause when not painted; delays of 10 ms or less play at
  100 ms, as browsers do.

## The document view

The document view (`src/page/docimages.c`) decodes images within a
viewport above or below the visible area before they scroll in,
invalidates only the rectangles of images that changed, and passes
pixels to layout through the image callback. A pending image is an
empty box; a failed one shows its alt text.

[`layout.md`](layout.md) says what an arriving image costs layout.
[`script-requests-and-cors.md`](script-requests-and-cors.md) says when
an image may be read back by script.

## Tests

`tests/image` has one test per format with committed vectors and
references made by the host's own decoders (the generator scripts and
their headers say how).

- PngSuite and the libwebp test images are fetched into `build/` by
  `tests/image/fetch-pngsuite.sh` and a `git clone` of
  `https://chromium.googlesource.com/webm/libwebp-test-data` into
  `build/webp-testdata`; their tests skip without them.
- To fuzz longer, build with the sanitizers
  ([`building.md`](building.md)) and run
  `build/asan/tests/image/fuzz_image ITERATIONS SEED
  tests/image/vectors/*/*`.

## Not done

See [`known-gaps.md`](known-gaps.md#images).
