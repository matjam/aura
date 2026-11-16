# Layout

This document covers `src/layout`: how to time layout and painting,
what an incremental layout reuses, the layout rules that were worked
out against Blink and the specifications, and grid layout. Read it
before changing the box tree, a formatting context, relayout or text
painting. Fonts and shaping are in [`fonts.md`](fonts.md); scrolling by
moving pixels is in [`browser-chrome.md`](browser-chrome.md); the
cascade is in [`css.md`](css.md).

## To time layout and painting

Build a Release tree ([`building.md`](building.md)) and run
`build/rel/src/platform/host/aura-host layout-bench PAGE
--bundled-only`. It prints, as CPU time:

- a layout from scratch at two widths (`-w`, `-w2`);
- a width change in the layout alone and through the document view
  (style included), and a height change;
- what each change cost (box tree builds, layout passes, intrinsic
  width runs, subtrees reused);
- painting: page downs over new content, the same pages again, a
  32-pixel strip and a one-pixel paint (the cost that does not depend
  on the area).

`--phase relayout|resize|paint|paint-cold|strip --repeat N` runs one
step N times for a profiler (`sample aura-host 3`). To count the passes
a window resize costs, run `aura-host chrome --tab PAGE --do
resize:W,H`.

Host timings do not predict the guest's; read
[`performance.md`](performance.md) before tuning.

## To add a layout case

Cases are in `tests/layout/cases/*.txt` (format in `test_layout.c`; the
test loader serves `data:` URLs, so cases can use images). Where a rule
below names Blink, it comes from reading Blink's source and from CSS
specifications, not from running Chrome beside Aura.

## Incremental layout

**A viewport change** is an incremental layout.

- The document view restyles and calls `au_layout_styles_changed`, not
  `au_layout_tree_changed`; the box tree is rebuilt only when a style
  difference needs it (display, position, float, overflow, generated
  content) or an element that had `display: none` is shown (layout
  keeps the elements it left out in `AuLayout.hidden`).
- Layout then reuses every independent subtree whose constraints did
  not change, and keeps intrinsic widths, which do not depend on the
  viewport.
- Lengths in viewport units reach layout as style changes.
- The restyle itself is skipped or partial when the cascade says so
  (see [`css.md`](css.md), "A viewport change").

**An image change** (`au_layout_images_changed`: a size became known,
an image failed) keeps the box tree.

- Each image box asks the loader again (`l_image_refresh`); a box whose
  size depends on the image is marked for layout, and one with a width
  and height in pixels is only repainted.
- The tree is built again only while its shape depends on images
  (`AuLayout.images_shape`: an object or embed element with a resource,
  a list-style-image that is not sized yet).
- The document view repaints the boxes that show a changed image
  (`au_layout_image_users` names the boxes with it as a background or
  mask) and the whole page only when layout changed geometry.

**Generated text** (a pseudo-element's content, a marker's number) is a
text box whose node is the element; `current_style` in `layout.c` finds
its style through the pseudo-element box above it.

**Overflow** (`l_finish_overflow`) leaves out an absolutely positioned
box whose containing block is outside the box: it is laid out later, so
its geometry there is the previous layout's.

**The horizontal scroll bar** keeps its state across a change of the
page area's width (`relayout` in `browser.c`), and `check_hscroll`
corrects it after the layout. Deciding from the old content width cost
a second pass on every resize of a page that follows the window.

## Costs that must stay linear

To check that layout time follows the size of a page, run
`build/host/tests/layout/test_layout_scaling -v` (ctest:
`layout_scaling`). It lays 22 generated pages out at a size and at four
times that size and fails when the larger takes more than ten times as
long. To cover a new pattern, add a generator to `patterns` in
`tests/layout/test_scaling.c`. A fuzzer's slow-page check finds such a
cost only when it happens to make a large page.

Three costs grew with the square of the content and were found by the
fuzzers' slow-page checks (a Domato page of 0.5 MB took 4 minutes 35
seconds on the host, and 3.9 seconds afterwards; a `fuzz_layout` input of
52 KB took 65 seconds, and 0.3 afterwards):

- **Splitting a text segment** (`split_seg` in `inline.c`). A text that
  wraps between any two characters (`overflow-wrap: anywhere`,
  `word-break: break-all`, a narrow column) is split once per line.
  The new segment went into the middle of the paragraph's segment
  array, which moved every later segment. The segments of finished
  lines are not read again, so the split now uses the slot before the
  line and moves the line's own segments down one (the first line of a
  paragraph still inserts). Keep any index into the current line in
  step: the line start `i`, and `S.deferred`.
- **Measuring the rest of a split text.** The rest was measured to its
  end at every split. A rest longer than `LAZY_TAIL_BYTES` is now
  marked `SF_LAZY_W` and measured by `seg_width` when a line has room
  for it; until then `seg_width` only says that it does not fit, at a
  cost that follows the room. Read a text segment's width through
  `seg_width` in the line breaker, never `s->w`.
- **Floats** (`block.c`, "Floats"). Every line and every float asked the
  formatting context's float list for the room in a band and for the
  next float's end, and each answer walked the whole list. Beyond 32
  floats the list now keeps two indexes: a binary tree over the list
  (the tops never decrease along it) that holds the lowest bottom under
  each node, and the bottoms in sorted runs. The summaries `max_top`,
  `bottom` and `side_bottom` answer the rest. A build with
  `-DAURA_FLOATS_CHECK` compares the indexes with the walk
  ([`fuzzing.md`](fuzzing.md)). Many floats that overlap one band (a
  row of thousands, each a fraction of a pixel wide) still cost their
  number per question.

Measured on the host (plain build, CPU time, 2026-11-15):

| Page | Before | After |
|---|---|---|
| 8,000 sibling floats | 0.251 s | 0.019 s |
| 6,000 floats on both sides | 0.312 s | 0.015 s |
| 6,000 floats in one paragraph of text | 0.210 s | 0.016 s |
| a word of 80,000 characters in a five-character column | 17.6 s | 0.017 s |
| the same word with 10,000 inline boxes after it | 19.3 s | 0.028 s |

A fourth was found by reading: `l_layout_abspos_for` in `layout.c` took
each absolutely positioned box out of the queue by moving every later
entry down. It now clears the entry and closes the gaps once at the end
(not measured before the change; 32,000 positioned spans in one
paragraph take 0.15 s). `l_queue_abspos` still looks through the queue
for a box that is queued a second time (a table lays its cells out
twice).

At four times the test's sizes (`test_layout_scaling -v -x 4`) every
pattern stays between 3.6 and 5.8 times its smaller size.

## Text painting

- A face caches the glyph of each code point below U+0250 and recent
  kerning pairs (`AuFontCache` in `font_internal.h`).
- The glyph cache stores masks with the text coverage table already
  applied; `au_draw_coverage` draws them, `au_draw_mask` still applies
  the table for other callers.
- The rasterizer uses stack work areas for paths up to 62 pixels wide
  and writes masks without the span callback.
- Layout shapes into one kept glyph run (`AuLayout.run`).

## Layout rules

### Font metrics are whole pixels

`l_font` rounds a face's ascent, descent and line gap each to a whole
pixel, as Blink's `SimpleFontData::PlatformInit` does, so `line-height:
normal` is their sum (16px Aura Sans, with Arial's metrics: 14 + 3 + 1
= 18px, the height Chrome gives 16px Arial). The leading above an
inline box is half the difference to the line height rounded down, the
rest goes below (`half_leading` in `inline.c`, Blink's
`CalculateLeadingSpace`). Fallback faces are rounded the same way in
`au_glyph_run_fallback_extents`. [`fonts.md`](fonts.md) says how
fallback faces change a line's height.

### Line breaking

- A `<wbr>` is a break opportunity whatever `white-space` says (Blink
  and Gecko break there inside nowrap text).
- Emoji break like ideographs (UAX #14 class ID): before and after
  each, and between a letter and an emoji.
- `l_break_class` gives `BK_EXTEND` to combining marks, variation
  selectors, ZWJ, emoji modifiers and tag characters, which stay with
  what precedes them (and a ZWJ joins what follows), and `BK_RI` to
  regional indicators, which break only between pairs.
- The emoji set is the pictograph blocks U+1F000 to U+1FAFF and the BMP
  characters with emoji presentation; it is not generated from the
  Unicode data files.

### Containing blocks

A box with a transform, `translate`, `rotate`, `scale`, `perspective`,
a filter, `backdrop-filter`, layout or paint containment, or
`will-change` naming one of those is the containing block of its
absolutely positioned and fixed descendants (`l_style_contains_fixed`,
`LF_TRANSFORM`). A fixed box under one scrolls with the page.

### The top layer

A dialog opened with `showModal()` is in the top layer (CSS Positioned
Layout 4, section 6). `showModal()` sets `AU_STATE_MODAL` on the
element (`N.setModal` in `prelude_html.js`); `close()` and removing the
`open` attribute clear it (`au_element_remove_attr`). The state makes
`:modal` match, so the UA sheet's `dialog:modal` rule makes the dialog
fixed and centres it in the viewport.

- To test whether a box is in the top layer, call `l_top_layer`: the
  box of an element with `AU_STATE_MODAL`, or of its `::backdrop`, when
  it is absolutely positioned or fixed.
- Its containing block is the initial containing block whatever its
  ancestors (`l_queue_abspos`), so no ancestor clips, moves or fades it.
- `l_build_stacking` makes it a stacking context in the root's list,
  and `sort_layers` puts it after every z-index, each element just
  above its backdrop. Painting and hit testing read the same lists.
- `::backdrop` gets a style only for an element in the top layer
  (`attach_pseudos` in `cascade.c`; such an element's style is not
  shared). `make_backdrop` in `boxtree.c` makes its box a child of the
  element's box; a backdrop that the page makes static or relative gets
  no box.
- A click on the backdrop goes to the dialog, as in Chrome.

The cases are in `tests/layout/cases/top_layer.txt`; a `script` line
before a case's HTML runs its scripts.

### Rounded clips

A box with `border-radius` and overflow clipped on both axes clips
positioned descendants to its rounded padding box: `layer_origin` in
`draw.c` paints the layer into a group that `layer_clips_pop` masks,
unless the layer's ink stays clear of the corners.

### `background-attachment: fixed`

It positions a layer against the viewport on any box. Once a box has
painted one, `au_layout_scroll_damage` returns -1 and a scroll repaints
the page.

### Clearance

A block placed by clearance takes the top margins of its first children
into its own top margin, which lies above the edge clearance placed
(`LMargin.absorb` in `block.c`); an empty cleared block still lets the
next block's margin apply.

### Images in layout

- `object-fit` and `object-position` are applied when an image box is
  painted.
- `list-style-image` makes the marker an image box with a 7px gap after
  it (Blink's marker padding), or the `list-style-type` marker when the
  image fails.
- `img_source` in `boxtree.c` chooses among `srcset` candidates for one
  device pixel per CSS pixel (the lowest density of at least 1; width
  descriptors against `sizes` and the viewport) and among the
  `<source>` elements of a `<picture>` (media and type), and the
  natural size is divided by the candidate's density.
  `au_layout_image_source` gives the document view the same choice.
- A choice that depends on the viewport makes a viewport change build
  the box tree again.

### Text offsets for script

`au_layout_text_pos_from_dom` and `au_layout_text_pos_to_dom`
(`selection.h`) convert between a text node's UTF-16 offsets and
`AuTextPos` by processing the node's data as the box tree did
(`l_text_process` with its offset map), and `au_layout_range_rects`
gives a range's rectangles per line.

## Grid

To change grid layout (`display: grid`, `inline-grid`), work in
`src/layout/grid.c`. It is a formatting context like flex: `l_fc_ops`
in `layout.c` dispatches `LI_GRID` boxes to `l_grid_layout` and
`l_grid_intrinsic`, and the box tree treats a grid container's children
as it treats flex items (blockified, text runs wrapped in anonymous
items, `LF_FLEX_ITEM | LF_BFC`, so `z-index` makes a stacking context
and incremental layout reuses an item whose constraints did not
change). The grid properties share the flex style group
(`AuStyleFlex`), so a change relays the box out without a tree rebuild.

### Order of a layout

1. `grid_build` expands the track lists (`repeat()`, `auto-fill`,
   `auto-fit`, line names), reads the named areas, resolves each item's
   lines (`resolve_lines`) and places the items (`place_items`:
   definite items, items locked to a row, then the auto-placement
   cursor, sparse or dense, over an occupancy bitmap). Implicit tracks
   before the explicit grid shift every index, so tracks count from 0.
2. `size_axis` sizes columns.
3. Each item's width is found (`item_width`).
4. `size_axis` sizes rows.
5. `position_axis` applies `justify-content` and `align-content`.
6. Each item is laid out and aligned in its area.

### Track sizing

Track sizing follows section 11 of the specification: items are taken
by span, each phase distributes an item's extra space to the tracks it
spans (`dist_item`, up to their limits by `water_fill`, then beyond),
then tracks are maximized, flexible tracks expanded (`find_fr`) and
auto tracks stretched. `mode` 1 and 2 size under a min-content and a
max-content constraint for the container's intrinsic widths.

### Measurements, percentages and positioned children

- **Measurements.** An item's column contributions come from one
  `l_outer_intrinsic` call (kept by `LF_INTRINSIC_OK`), its row
  contribution from one `l_measure_content` call at its used width
  (kept by the measure cache). A stretched item is laid out a second
  time at its final height, as in flex.
- **Percentages.** A percentage track or gap behaves as auto while the
  container's size in that axis is unknown (intrinsic widths, an auto
  height); the rows are then sized again against the height found.
- **Absolutely positioned children** of a grid container that is their
  containing block are positioned against the area their grid lines
  name; an `auto` line is the padding edge. `abs_area` records the area
  in `AuLayout.grid_abs`, and `layout_abspos` in `layout.c` takes it
  with `l_grid_abspos_area`.

### Limits

An axis has at most 1000 tracks (`G_MAX_TRACKS`): `repeat` counts stop
there, lines beyond are clamped into the last track and spans are cut.
Line names are capped at 4096 per axis and areas at 1024. Keep new
loops bounded by these.

### Grid tests

`tests/layout/cases/grid*.txt` (sizing, placement, alignment,
integration; expected values are worked out from the specification, and
a comment says where a browser was not consulted),
`tests/css/cases/grid.txt`, and `tests/page/samples/grid.html`, which
the relayout and scroll-shift tests load.

## Tests

- `tests/layout/test_relayout.c` resizes each sample page and several
  inline pages through ten viewports and compares boxes and pixels with
  a fresh document view at each size, then checks the counters (no tree
  build or intrinsic run for a height or width change, a new measure
  after a text or font size change).
- `tests/paint/test_shape_cache.c` covers the face caches and the glyph
  cache.
- `test_image_arrival_cost` in `tests/page/test_page.c` checks that an
  arriving image builds no tree, lays out only when its size matters
  and invalidates the boxes that show it.

## Measured

A Wikipedia article of 18,053 boxes, host, CPU time:

| Step | Now | Before |
|---|---|---|
| Width change in layout | 2.6 ms | 3.7 ms |
| Paint of a 1440x900 view | 0.92 ms | 1.22 ms |
| First paint of a page of new text | 1.36 ms | 1.68 ms |
| A 32-pixel strip | 0.046 ms | 0.061 ms |
| A one-pixel paint | 0.005 ms | |

Painting already costs in proportion to what it shows. A width change
through the document view took 18 ms, of which the restyle of every
element was 15 ms; on live Wikipedia (`aura-host chrome --do
resize:W,H`) a resize that crosses no media query breakpoint now spends
1 to 2 ms in style and layout, and one that crosses a breakpoint 15 to
17 ms.

The same article in the guest (emulated, stock base, 600 pixels wide):

| Step | Now | Before |
|---|---|---|
| The last style and layout pass of the load | 465 ms | 11,251 ms (before the pooled allocator) |
| A whole-page paint | 4 to 5 ms | 54 ms |
| A page down | 4 to 11 ms | 349 ms |
| A window zoom | two passes of about 280 ms each | |

Grid, host (Release, CPU time): a 12-column grid of 200 items with
spans of 1 to 6 (803 boxes) lays out from scratch in 0.93 ms at 1440
pixels and 1.04 ms at 656; a width change takes 0.6 to 0.7 ms.

## Acid2

Acid2 (http://acid2.acidtests.org/#top, compared with its reference
image at the scroll position of `#top`): the rows that still differ are
four rows of the eyes (rows 51, 53, 55 and 57 of the 168-pixel face:
the Adam7 PNG shows the row above repeated in image rows 1 to 9 in
`aura-host render`, though `aura-host image` decodes the file
correctly; the cause is not found) and two pixels per row along the
nose's diagonal border joins (rows 72 to 95, where the red background
shows through the antialiased seam between two border colours).

## Not done

See [`known-gaps.md`](known-gaps.md#layout).
