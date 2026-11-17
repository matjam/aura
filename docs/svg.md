# SVG

This document covers `src/svg`: the XML parser, the static SVG renderer
and how SVG appears inline, as an image and as a mask. Read it before
changing the renderer or when an SVG draws wrongly.

`src/svg/svg.h` describes what the renderer supports and what it does
not. One renderer serves inline SVG and SVG documents; it reads the
elements' computed styles, so page CSS applies to inline SVG, and
presentation attributes reach the cascade as presentational hints
(`svg_hints` in `src/css/ua.c`).

## To try a file

Run `build/host/src/platform/host/aura-host image FILE.svg --target WxH
-o out.png`. `tests/page/samples/svg.html` shows each feature.

## Inline SVG

Layout makes an outer `svg` element a replaced box (`LR_SVG`): its size
comes from the width and height attributes, its ratio from them or the
viewBox, else 300 x 150. `paint_svg` in `src/layout/draw.c` draws it
into the content box with `au_svg_paint`, at the size it is shown.
References (`use`, gradients, clip paths, masks) resolve through a
document id index the layout keeps until the tree changes.

## SVG as an image

`src/image/svgimg.c` is the codec: markup is sniffed as SVG, the bytes
are held to the end, and the document is drawn at the target size
rather than reduced. The cache keeps SVG bytes and draws the image
again when it is shown larger or in another shape; layout asks for each
paint size through `AuLayoutEnv.image_use`. An SVG image loads nothing:
only `data:` URLs in `<image>` draw, nested SVG to depth 2.

An SVG image with a viewBox but no width or height has no natural size,
only a ratio. The codec marks it (`AuImageInfo.ratio_only`, read by
layout through `AuLayoutEnv.image_ratio_only`), and layout sizes it as
browsers do: it fills the containing block's width at the viewBox ratio
(CSS 2.1 10.3.2), takes its width from a set height, and contributes 0
to shrink-to-fit widths. A flex item fills the flex container. The
300 x 150 size the codec reports is used only for drawing when nothing
else gives a size, and for an SVG with neither size nor viewBox.
`tests/layout/cases/images.txt` checks each case against Chrome.

## Masks

`mask-image` and `-webkit-mask-image` layers draw into a canvas mask
group (`paint_mask` in `draw.c`), alpha by default, luminance with
`mask-mode: luminance`.

## Limits

- The XML parser caps depth (256), nodes (100000) and attributes (256),
  skips the doctype and never expands entities beyond the predefined
  five and character references.
- The renderer caps nesting (48), `use` depth (12) and cycles, elements
  per paint, href chains and text length.
- Clip pieces and text layout go on the heap because the renderer
  recurses.

## Tests

`tests/svg` holds `test_svg` (grammars, XML, pixels, the image
pipeline; `--bench` times 200 icons) and `fuzz_svg`. To fuzz longer,
run the sanitizer build's `fuzz_svg ITERATIONS SEED [SECONDS]
tests/svg/corpus/*` ([`building.md`](building.md)). Layout cases are in
`tests/layout/cases/svg.txt`.

## Not done

See [`known-gaps.md`](known-gaps.md#svg).
