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

**A viewport change** is an incremental layout, and so is **a style
sheet that arrives or changes** after the first layout.

- The document view restyles and calls `au_layout_styles_changed`, not
  `au_layout_tree_changed`; a box whose style difference changes the
  boxes (display, position, float, overflow, generated content, list
  markers, counters), an element that had `display: none` and is shown,
  and a `display: contents` element whose new style builds something
  else are rebuilt with their parent's subtree, as a DOM change is
  (below). Layout keeps the elements it built no box for, with the
  style they had, in `AuLayout.hidden`.
- Only a DOM change the document view did not record (`tree_dirty`,
  such as the parser's insertions after the first layout) builds the
  whole tree with the restyle.
- Layout then reuses every independent subtree whose constraints did
  not change, and keeps intrinsic widths, which do not depend on the
  viewport.
- Lengths in viewport units reach layout as style changes.
- The restyle itself is skipped or partial when the cascade says so
  (see [`css.md`](css.md), "A viewport change").

**A DOM change** (a child inserted or removed, text edited, an attribute
that a box reads, a control's state) rebuilds the smallest subtree that
holds it (`src/layout/rebuild.c`).

- The document view calls `au_layout_node_changed` with the parent of
  the change (the text node for a text edit). It records the node and
  nothing else, so it is safe while the DOM is mid-mutation. A node
  outside the view's document is ignored: its insertion is recorded
  when it comes.
- Before the next layout, `l_rebuild_dirty` finds for each recorded
  node the rebuild root: the nearest element, the node or an ancestor,
  whose principal box is a block-level, non-anonymous block box that is
  not the root box, a table part, a caption or a legend, whose style is
  not inline-level, and that shares no counter or quote with content
  outside it. Below `display: none` the change is dropped, unless a
  replaced box above it shows its content (a select shows its options'
  text).
- Such a root's content starts after a collapsed space, the space after
  it collapses whatever it holds, the anonymous blocks and flex items
  its parent makes depend only on its being block-level, and no table
  grid outside it points into it. So its subtree can be built alone.
- Every root's old subtree is dropped first (a node that moved from one
  root to another must not keep its old box), then `l_build_subtree`
  builds each root with the walk the whole build uses and runs the
  table fixup, normalization and table grids on the new subtree. The
  new box takes the flags its parent's normalization gives (flex and
  grid items) and goes in place of the old one; it and every ancestor
  are marked for layout, and scroll offsets inside it are kept.
- The whole tree is built instead when a root's outer kind changes
  (inline-level, floated, positioned, a marker, a flex item, a list
  item), its counter properties change, the walk meets a counter or a
  quote from outside the subtree (`LX_CTR_OPEN`, below), the search
  reaches the document, or more than 64 separate nodes changed.
- Dropped boxes stay in the arena until the next whole build; when the
  arena grows past twice the size of the last whole build plus 256 KB,
  the next change builds the whole tree, which frees them.

**Counter scope.** A counter's scope is the element that creates it,
its following siblings and their descendants, so a subtree that uses a
counter created outside it cannot be built alone. The whole build
records, on each block box, whether its subtree uses a counter or a
quote from outside (`LX_CTR_OPEN` in `LBlock.xf`), and such a box is
never a rebuild root. A subtree build that meets a counter name the
document has used but cannot find inside the subtree, or any quote,
gives up and the whole tree is built. A list marker reads the
`list-item` counter only when it shows a number, so an unnumbered list
item does not tie its subtree to the list.

**To check that a rebuild equals a whole build**, turn on the
differential check: build with `-DAURA_INCR_CHECK` (every layout and
document view checks, and a difference aborts with the path of the
box), or call `au_layout_set_check` or `au_docview_set_check`. After
each update that rebuilt a subtree, `l_check_incremental` builds the
whole tree from the same document and styles in a second layout, lays
it out at the same viewport and compares the two trees box by box
(kind, node, style, flags, text, replaced content, geometry, overflow,
scroll extent, counter scope, lines, fragments and table grids), the
element box pointers and the list of elements without a box. A
difference in a tree of fewer than 300 boxes prints both trees
(`au_layout_dump`) under `AURA_INCR_CHECK`.
`au_docview_check_failures` returns the count and the first difference.
The check also runs after an update whose recorded changes needed no
rebuild, which catches a change dropped wrongly. Geometry (positions,
sizes, overflow, lines, fragments, the document size) is compared only
while nothing but subtree rebuilds has asked for layout since the last
whole build (`AuLayout.other_relayout`): a relayout after a viewport
change or a style change that keeps the boxes differs in the case
below, and the check would blame the rebuild for it. The tree itself is
always compared. To compare geometry anyway while working on that case,
set `geom` to true in `l_check_incremental`.

**A relayout must give what a layout from scratch gives.** These rules
keep it so:

- A block's content is reused (`can_skip`) only when its width, height,
  containing block height and the offset of its content box (padding
  plus border, `key_top` and `key_left`) are what they were. A
  percentage padding or border that changed moves the content even when
  the width did not.
- A reused block takes its content height from `key_ch`, and the measure
  cache (`m_res`) stores a content height, so a changed padding or
  border is added to it again.
- `l_mark_dirty` marks every ancestor: an ancestor laid out since the
  last mark (by the measure pass, say) has lost its mark, and stopping at
  the first marked one left it reused.
- `relative_pass` offsets every relatively positioned box after a
  layout. A block whose content was reused (`can_skip`, or the measure
  cache) keeps its descendants' positions, offsets included, so the pass
  does not descend into a block whose `skip_gen` is the current
  generation unless it was also laid out in it (`lay_gen`).
- An absolutely positioned box whose containing block is the initial
  one is laid out after the relative pass, so `relative_pass_late` gives
  the relatively positioned boxes inside it their offsets.
- A block that avoids floats (`place_bfc_root`) is first placed with a
  height of 1, not with the height a previous layout left in it, and is
  placed again with its real height when that height meets floats it
  fits beside.
- A table cell whose final height is definite is laid out again at that
  height when content below it has a percentage height, even when the
  height equals its content height: the first layout resolved those
  percentages against an indefinite height.

- A style change that needs only a repaint but makes or unmakes a
  stacking context or layer (z-index, clip-path, filter, isolation,
  mix-blend-mode, masks, contain: paint) builds the layer lists again
  (`stacking_style_differs`).
- An element adopted from another document (a frame's) loses its box
  pointer and its hover, active, focus and target states, which belong
  to the other document's layout and view (`set_doc_subtree` in
  `src/js/natives.c`).

One difference remains, so the differential check compares geometry
only after subtree rebuilds (`other_relayout`, below): overflow and the
document size are found before relative offsets are applied, so a
relatively positioned box that sticks out reaches them only at the next
relayout, which keeps the offset positions of reused content.
[`known-gaps.md`](known-gaps.md) has the reduced case.

**Shadow trees.** In a document with a shadow root, `find_root` and
`in_place` follow the flat tree (`au_flat_parent`: a slotted node's
slot, a shadow root's host), and a box whose node no slot takes any
more is not in place. The document view records a change to a host's
children, to a shadow root's children or to a subtree with a slot in a
shadow tree on the host's parent, so the host's boxes are built again
("Shadow trees" below). The differential check walks the flat tree.

The restyle after a container query round goes through
`au_layout_styles_changed` like any other ("Container query rounds"
below); a difference that changes the boxes is rebuilt there, not in
the whole tree.

To run the tests and fuzzers under it, configure a tree with
`cmake -S . -B build/check -G Ninja -DCMAKE_C_FLAGS=-DAURA_INCR_CHECK`
and run `ctest --test-dir build/check` or a fuzzer from it. The check
build is slower: a fuzzer runs fewer iterations in its time cap.

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

**Hidden text** (a web font in its block period,
[`fonts.md`](fonts.md#block-periods-and-font-display)) is laid out as usual
and not painted: `l_font_hidden` asks the env callback `text_hidden`
with the font's family list, weight and style, and caches the answer
in the `LFont` until `au_layout_text_visibility_changed` bumps
`AuLayout.hidden_gen`, which also asks for a repaint. Text decorations
of hidden text are not painted either.

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

### Container query rounds

To evaluate container queries after a layout, the document view runs
`container_rounds` (`src/page/docview.c`) at the end of
`au_docview_update`, only when the resolver has read a container
(`au_css_resolver_has_containers`). Each round:

1. asks `au_css_resolver_containers_changed` which containers changed
   size, and stops when none did;
2. restyles (`au_css_restyle`) and passes the changed styles to
   `au_layout_styles_changed`;
3. lays out again (`au_layout_update`).

The loop stops after `CQ_ROUNDS` (4) rounds. A chain of containers in
which each one's size depends on a query of the one above settles one
level per round, so a fifth level keeps the style of the previous
round. A page without `@container` rules or container units never
enters the loop.

`au_layout_css_container_size` gives the resolver the content-box size
of an element's block box (`LK_BLOCK` with `LF_HAS_LAYOUT`); an element
without one has no size, and its queries do not match until it has.

### Size containment

`container-type: size` and `contain: size` contain both axes,
`container-type: inline-size` the width (`l_size_contained`, which
returns bit 1 for width and bit 2 for height). It applies to flow,
flex and grid boxes that are not anonymous and not replaced.

- A contained width makes the box's intrinsic widths 0
  (`l_intrinsic`), so a shrink-to-fit or flex item container needs a
  width of its own.
- A contained height makes an auto height 0, plus padding and border
  (`l_layout_content`, and `finish_block` for in-flow blocks); an
  `aspect-ratio` still gives a height. The children are laid out and
  overflow.
- A change of `container-type` relayouts the box (`style_diff`).

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
- `overflow-wrap: anywhere` and the legacy `word-break: break-word`
  lower the min-content width to the widest character;
  `overflow-wrap: break-word` does not (CSS Text 3).

### Containing blocks

A box with a transform, `translate`, `rotate`, `scale`, `perspective`,
a filter, `backdrop-filter`, layout or paint containment, or
`will-change` naming one of those is the containing block of its
absolutely positioned and fixed descendants (`l_style_contains_fixed`,
`LF_TRANSFORM`). A fixed box under one scrolls with the page.

### The top layer

A dialog opened with `showModal()` and a showing popover are in the top
layer (CSS Positioned Layout 4, section 6). The document keeps the list
(`AuDocument.top_layer`, `au_top_layer_add` in `dom.c`) in the order
the elements were added; the element also carries `AU_STATE_MODAL` or
`AU_STATE_POPOVER_OPEN`, which make `:modal` and `:popover-open`
match. `N.setTopLayer` in the bindings sets both
([`forms.md`](forms.md), "Dialogs and popovers"). The UA sheet makes a
modal dialog and a popover fixed and centres them in the viewport.

- To test whether a box is in the top layer, call `l_top_layer`: the
  box of an element with either state, or of its `::backdrop`, when it
  is absolutely positioned or fixed.
- Any other position computes to `absolute` for such an element and
  its `::backdrop` (`css_compute_finish` in `compute.c`), so a page's
  `position: static` does not take a dialog out of the top layer.
- Its containing block is the initial containing block whatever its
  ancestors (`l_queue_abspos`), so no ancestor clips, moves or fades it.
- `l_build_stacking` makes it a stacking context in the root's list,
  and `sort_layers` puts it after every z-index, in the order of the
  document's list (`au_top_layer_index`), each element just above its
  backdrop. Painting and hit testing read the same lists.
- `::backdrop` gets a style only for an element in the top layer
  (`attach_pseudos` in `cascade.c`; such an element's style is not
  shared). `make_backdrop` in `boxtree.c` makes its box a child of the
  element's box. A popover's backdrop has `pointer-events: none`, so
  the page stays live around it.
- A click on a dialog's backdrop goes to the dialog, as in Chrome.
- An element that leaves the document leaves the list and loses both
  states (`top_layer_left`, called from `unlink_node`).

### Inert content

To test whether a node is inert, call `au_node_inert` (`dom.h`), or
`l_box_inert` for a box. A node is inert inside an element with the
`inert` attribute, and, while the document has a modal dialog, when it
is outside the topmost modal dialog of the top layer. Until a document
has had an `inert` attribute (`AuDocument.has_inert`) and while it has
no top layer, the test returns at once.

- Hit testing (`hit.c`) treats an inert box as `pointer-events: none`
  and passes over its subtree; only the root is still entered, since
  top-layer boxes are layers of its list. A point with only inert
  content under it has no target (`AuHitResult.element` is NULL), so
  no event, hover, link or contextual menu link reaches the page there.
- Selection (`selection.c`) gives no text position at a point over
  inert content, Select All takes only text that is not inert, and the
  text of a range, find and the painted highlight leave inert text out.
- Focus is in [`forms.md`](forms.md).

The cases are in `tests/layout/cases/top_layer.txt`; a `script` line
before a case's HTML runs its scripts.

### Shadow trees

To lay out a document with shadow trees, nothing extra is needed: the
box tree is built from the flat tree (`boxtree.c`, `Builder.flat`)
whenever `AuDocument.has_shadow` is set.

- A host's boxes come from its shadow root's children; its own children
  get boxes only where a slot takes them.
- A slot is `display: contents` (the UA sheet), so its assigned nodes,
  or its fallback children when nothing is assigned, lay out in the
  slot's parent.
- Text takes the style of its flat parent (`current_style`).
- Hit testing reports the element of the box under the point; the link
  of a hit is the nearest `<a href>` up the flat tree, so text slotted
  into a link of a shadow tree is a link. Script sees the result
  retargeted (`elementFromPoint`, [`javascript.md`](javascript.md),
  "Shadow DOM").
- Selection order (`au_node_order`) follows the flat tree; find in page
  searches the text boxes, so it finds text in shadow trees.
- `src/dom/shadow.c` holds the flat tree walks: `au_flat_first_child`,
  `au_flat_next_sibling`, `au_flat_parent`, `au_flat_next` (preorder,
  iterative) and `au_node_in_flat_tree`.

The cases are in `tests/layout/cases/shadow.txt` (declarative shadow
roots, so they need no script).

### Hit testing follows the paint order and its clips

`hit.c` walks the boxes in the reverse of the paint order in `draw.c`
and must apply the same clips, or a click lands on a box that is not
drawn there.

- A layer's clips come from two places. `layer_origin` applies the
  clips of the boxes between the layer and its stacking context;
  `hit_layer` applies the stacking context's own overflow clip to every
  layer in its lists, as `paint_layer` does with `push_clip` before
  `paint_layer_list`. A point outside a stacking context's clip reaches
  none of its positioned descendants.
- A box that clips and is a stacking context (a transform, a z-index,
  opacity) holds its positioned content in its own layer list. Without
  its clip in hit testing, content moved outside it took the clicks of
  its neighbours: reCAPTCHA's 4x4 image tiles each show the whole photo
  moved by -100% steps inside a wrapper with `transform: scale(1)`, and
  every click went to the last tile.
- A transformed layer is tested with the point mapped back through its
  matrix, so a scaled clip clips the point where it is drawn.

The cases are in `tests/layout/cases/overflow.txt` ("a stacking
context's overflow clip ..."), and `test_image_tiles` in
`tests/frames/test_frames.c` covers the whole challenge in a frame.

### URLs of any length

To resolve a URL in layout, call `l_resolve_url` and release the result
with `l_url_free`: it resolves into the caller's buffer, or into a block
from `au_alloc` when the result is longer. In the document view, use
`au_url_resolve_dup` for image, font, script and style sheet URLs. A
`data:` URL is often several kilobytes; the 2048-byte buffers these
paths used dropped a long background image without asking for it and
cut a long `<img>` URL short (reCAPTCHA's check mark is a 3978-byte
`data:` URL in its style sheet). The case is in
`tests/layout/cases/images.txt`.

`l_resolve_url` takes a base: pass the `base_url` of the value's
`AuCssBlob` (or `AuBgLayer.base_url`) for a `url()` from a style sheet,
and NULL for an attribute or inline style, which resolve against the
document ([`css.md`](css.md), "Sheets in a document").

### Snapped backgrounds

`paint_background` fills a square-cornered background colour over whole
surface pixels (`snap_device` in `draw.c`), as browsers snap a box's
background. A 1px rule at a fractional position (Cornell Chronicle's
gold bar under the date, `header::after` at `height: 1px`) is then one
row in its own colour instead of two pale rows that read as grey.
Rounded backgrounds and images are not snapped. `test_background_snapped`
in `tests/page/test_page.c` covers it.

### Background images without a natural size

To size a background image whose natural width or height is missing,
`draw_bg_layer` asks the layout environment's `image_missing` callback
which of width, height and ratio the image lacks
(`AU_LAYOUT_IMG_MISSING_*`, from `AuImageInfo.missing`), and follows
CSS Images 3 default sizing. With `background-size: auto` and only a
ratio (an SVG with a `viewBox` but no `width` or `height`), the image
is sized as `contain` in the origin box. With no ratio either, it fills
the origin box. When one side is given, the other comes from the ratio,
or else from the origin box. Cornell Chronicle's wordmark is such an
SVG; it drew at its 300x150 fallback size before. An environment
without the callback keeps the old natural-size behaviour.
`test_background_svg_ratio_only` in `tests/page/test_page.c` covers it.

### Scroll bars of scrollers

`paint_scrollbars` in `draw.c` draws a scroller's bars over its padding
box; they take no layout space. `scrollbar-width: none` draws none (the
box still scrolls), which is how Tailwind's `scrollbar-hide` hides the
bar under Reddit's post action row, and `thin` draws 5 px bars instead
of 8 px. `::-webkit-scrollbar` rules are not read. The cases are in
`tests/layout/cases/overflow.txt`.

### Rounded clips

A box with `border-radius` and overflow clipped on both axes clips
positioned descendants to its rounded padding box: `layer_origin` in
`draw.c` paints the layer into a group that `layer_clips_pop` masks,
unless the layer's ink stays clear of the corners.

### `clip-path`

`clip_path_shape` in `draw.c` turns `inset()`, `circle()`, `ellipse()`,
`polygon()` and a box keyword alone into a path against the reference
box (border box by default; lengths may be `calc()` trees, which
`au_len_resolve` evaluates against the box), and `paint_layer` paints
the layer into a group masked by it. A shape with no area (the visually-hidden
`polygon(0 0, 0 0, 0 0)` that AddToAny and others put on screen-reader
labels, a zero radius, an inset past the box) paints nothing. The
cases are in `tests/layout/cases/overflow.txt`; what is not done is in
[`known-gaps.md`](known-gaps.md).

The old `clip: rect()` property goes the same way (`clip_rect_build`):
on an absolutely or fixed positioned box it is a rectangle offset from
the border box's top-left corner, and the WordPress visually-hidden
`rect(1px, 1px, 1px, 1px)` paints nothing.

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
- `border-radius` clips an image to its content box with the radii
  less the border and padding (`content_radii` in `draw.c`); a canvas,
  frame or SVG image is not clipped yet ([`known-gaps.md`](known-gaps.md)).
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
- A replaced flex item's automatic minimum width is its min-content
  width, not its specified width: the width transferred through its
  ratio from a definite height, or 0 when it has no natural width
  (`min_m` in `flex.c`). An inline SVG of `width:32px; height:20px`
  with a square viewBox shrinks to 20 in a 20px flex box, as AddToAny's
  share icons need. A replaced box with no ratio record (a canvas, an
  inline SVG) takes its height from the ratio of its attributes or
  viewBox when it is laid out at a flexed width. The cases are in
  `tests/layout/cases/flex.txt`.

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
