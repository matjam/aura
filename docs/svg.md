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

To show an image in a `foreignObject`, put an HTML `<img>` in it: the
renderer draws the first `<img>` of the object's subtree
(`au_svg_foreign_img` in `src/svg/render.c`, which looks at the first
64 nodes) stretched over the object's x, y, width and height, through
the object's mask and clip path, in document order with the other SVG
elements. Discord's avatars and server icons are drawn this way. The
image is requested when the SVG first paints. An SVG image (a file, not
inline) puts every element in the SVG namespace, so its `foreignObject`
draws nothing. `test_foreign_object_img` in `tests/page/test_page.c`
checks it.

## HTML in a foreignObject

Any other HTML in a `foreignObject` of inline SVG is laid out by the
layout engine and painted by the renderer in the object's place.
Discord's server rail draws its Home button, folders and servers
without an icon this way: a div with a background and an inline svg
icon or a text acronym, centred, in a masked object.

- Building: each svg box the box tree builds is noted
  (`l_foreign_note`). At the next update, `l_foreign_sync` in
  `src/layout/foreign.c` builds a root for each `foreignObject` under
  the svg element (not those inside another object): an anonymous block
  container with the object's style and no edges, a containing block
  for positioned and fixed descendants, holding the boxes of the
  object's children (`l_build_foreign` in `boxtree.c`, the box tree's
  own walk). The roots are kept in `AuLayout.foreign`, not in the box
  tree, and the elements in them get no box pointer.
- Layout: each root is laid out after the root box, at the object's
  width and height in its user space (`au_svg_foreign_size`).
- Paint: the renderer calls `AuSvgEnv.foreign` for an object with no
  `<img>`, inside the object's opacity, mask and clip path and with a
  clip to its box. When the user space only moves the object, the root
  is painted in place with the box painter (`svg_env_foreign` in
  `src/layout/draw.c`); when it is scaled or rotated, the root is
  painted at CSS pixels into a layer drawn through the transform.
- Changes: a DOM change below an object finds no box until the svg's
  box, so the subtree the svg is in is rebuilt, and its new svg box gets
  new roots; the old ones go with the old svg box. A restyle compares
  the roots' boxes as it compares the tree's: a change that keeps the
  boxes swaps the style in (marking the box for layout when geometry
  changes), and one that changes what is built marks the root stale,
  built again at the next update. Damage is the whole view.
- Limits: 1024 roots per layout (`L_FOREIGN_MAX`); an object inside an
  svg inside an object gets a root, one more level deep does not
  (`L_FOREIGN_DEPTH`).

`test_foreign_object_html` in `tests/page/test_page.c` checks the
background, a centred svg icon and text through a mask, a nested
object, a scaled user space, a restyle, a change of display and a
removed object, with the differential check on.

## SVG as an image

`src/image/svgimg.c` is the codec: markup is sniffed as SVG, the bytes
are held to the end, and the document is drawn at the target size
rather than reduced. The cache keeps SVG bytes and draws the image
again when it is shown larger or in another shape; layout asks for each
paint size through `AuLayoutEnv.image_use`. An SVG image loads nothing:
only `data:` URLs in `<image>` draw, nested SVG to depth 2.

An SVG image with a viewBox but no width or height has no natural size,
only a ratio. The codec records which of a natural width, height and
ratio the document lacks (`AuImageInfo.missing`, read by layout through
`AuLayoutEnv.image_missing`; a background image uses the same flags,
see "Background images without a natural size" in
[layout.md](layout.md)). Layout sizes an image with only a ratio as
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
