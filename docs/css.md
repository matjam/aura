# CSS

This document covers `src/css`: how to restyle after a change, what a
change restyles, how sheets are parsed and shared, and how to measure
the cascade. Read it before changing the parser, selectors, the
resolver or invalidation, or when a page restyles more than it should.

`src/css/css.h` is the interface: sheets, the resolver, computed
styles, invalidation, selectors. `css_int.h` is shared by the files of
`src/css` and its tests only.

## To restyle after a change

Call `au_css_invalidate` (before or after the change) and then
`au_css_restyle`.

- For a state bit use `au_css_invalidate_state` with the `AU_STATE_*`
  bits that changed, on every element whose bits changed.
- For hover and active use `au_css_move_state`, which sets the bits and
  reports only the elements below the common ancestor of the old and
  the new element.

## To add an attribute that styles depend on

To add an attribute that a pseudo-class or a presentational hint reads,
add it to `attr_state` or leave it out of `attr_never_hint` in
`cascade.c`.

## To measure

- To time hover moves over a page, run
  `build/host/tests/css/test_css_invalidate --bench [--verify]
  PAGE.html [SHEET.css ...]`: it prints elements restyled and time per
  move, first with the old coarse invalidation, then targeted.
- To see what sheets cost, run `build/host/tests/css/css_bench --mem
  FILE.css...` (macOS only): it prints the heap each parsed sheet keeps
  and the index size.
- To see where that memory is, run `css_bench --where [--style
  PAGE.html] FILE.css...`: bytes and blocks per structure (rules,
  selectors, components, media lists, parsed declaration blocks, the
  text, token lists), for the host and as the struct layouts give them
  on 32-bit PowerPC. With `--style` the page is styled first, so the
  blocks of the rules that matched are parsed.
- To time a page, run `css_bench --page PAGE.html FILE.css...`: parse,
  index and first style times and the heap after each. Give a file as
  often as the page links it.

## What a change restyles

When the rule index is built (`deps_build` in `cascade.c`), every
selector is analysed for the states it reads and the classes, ids and
attributes it names, and for where the elements it can style lie from
the element that changes: the element itself, its descendants, its
following siblings, or the ancestors and preceding siblings a `:has()`
argument can anchor on.

- A change marks only those (`AU_STATE_STYLE_SELF_DIRTY` for one
  element, `AU_STATE_STYLE_DIRTY` for a subtree).
- A class, id or attribute that no selector names, no presentational
  hint reads and no `attr()` or pseudo-class depends on restyles
  nothing.
- Selectors the analysis cannot follow (`:nth-child(... of S)` with a
  state or name in S, a complex selector nested inside a `:has()`
  argument) restyle the document.
- Child insertions and removals restyle what the selectors can reach
  (see below).

**Child insertions and removals** go through
`au_css_invalidate_children`, which the document view calls from the
`child_changed` mutation hook with the parent, the child and the
neighbours it had. It is scoped as Blink scopes `ChildrenChanged`, from
masks that `deps_build` records for each selector outside `:has()`:

- the inserted subtree always;
- the parent, for `:empty` (`st_empty`);
- the following siblings, for `:nth-child`, `:nth-of-type`,
  `:first-child`, `:only-child` and `~` (`st_forward`, `st_ind`): the
  sibling rule goes on the first following sibling only, and the
  descendant and self rules on each, so a long list costs its length;
- the preceding siblings, for `:last-child`, `:nth-last-*` and
  `:only-*` (`st_back`);
- the next element sibling, for `+` (`st_adj`);
- the `:has()` anchors among the parent, its ancestors and their
  preceding siblings (`has_anchors`, one entry per `:has()` argument
  with the key its anchor compound names); an argument with an
  unanchored relative selector restyles the document;
- form owners above the parent (form, fieldset, select, option,
  textarea) when a rule reads `:valid`, `:checked`, `:indeterminate` or
  similar; an inserted control when a rule reads `:default` restyles the
  document.

A text change goes through `au_css_invalidate_text`, which restyles the
parent for `:empty`, the `:has()` anchors above it when a `:has()`
argument reads `:empty`, and the form owners above it (a textarea's
`:placeholder-shown`).

To check a targeted restyle, turn the differential check on
(`au_docview_set_check`, or build with `-DAURA_INCR_CHECK`): after each
restyle that followed a child or text change, `au_css_check_restyle`
runs a whole style pass and compares every element's computed values
with the targeted result.

**Class and id changes** need the names that changed. `src/dom` notes
them in `AuDocument.style_names` when the attribute changes, and the
resolver resolves the elements reported for class or id when the
restyle starts (the DOM mutation hook reports before the change).

## A viewport change

The rule index records every media query it evaluates with its result
(`media_dep` in `cascade.c`), and a computed style that used a viewport
unit has `AU_STYLE_F_VIEWPORT_UNITS`.

- `au_css_resolver_set_device` with a device that differs only in the
  viewport size evaluates the recorded queries again: when none
  changed, the index is kept and `au_css_restyle` computes only the
  elements whose style (or pseudo-element style) has the flag, and what
  inherits from them; with no such style it computes nothing.
- `au_css_resolver_viewport_restyle` says which of the three applies,
  and the document view skips the style pass on `AU_CSS_VIEWPORT_NONE`.
- Any other device difference, or a media query that changed, restyles
  the document.
- A container query reads a container's size, not the viewport, so a
  viewport change reaches it through layout: "Container queries" below.
- `test_css_invalidate --viewport DIR` (ctest `css_viewport`) resizes
  pages through fourteen sizes and compares every computed value with a
  document styled from scratch.

[`layout.md`](layout.md) says what layout reuses after the restyle.

## The colour scheme

`prefers-color-scheme` reads `AuCssDevice.dark`. To change it on an
open document, call `au_docview_set_dark(dv, dark)`; the browser does
this for every tab through `au_page_set_dark` when the Appearance
preference or the system setting changes ([`browser-chrome.md`](browser-chrome.md),
"Appearance"). It:

- sets the device, which restyles the document (a device difference
  other than the viewport size);
- starts the `<link>` and `<style>` sources whose media query now
  matches (`undefer_sheets`), without a reload;
- queues a `change` event on every `matchMedia` list whose result
  changed (`au_docscript_media_changed`, which runs `P.mediaChanged` in
  the prelude at the next script turn);
- passes the flag to every frame (`au_frames_settings_changed`); a frame
  made later copies it from its parent;
- passes it to layout (`au_layout_set_dark`) for `<picture>` sources
  with a media query.

`test_color_scheme` in `tests/frames/test_frames.c` covers all of these.
To render a page dark on the host, pass `--color-scheme dark` to
`aura-host`.

## Pausing a style pass

To let a long style pass give the window time, set a yield hook with
`au_css_resolver_set_yield(resolver, fn, ctx)`. The style pass calls it
every 32 elements, between two elements, so the hook may switch
coroutines (the browser parks the page there,
[`browser-chrome.md`](browser-chrome.md), "Scrolling while script
runs"). The hook must not change the document or its style sheets.

## Equal styles keep their object

A style pass compares each newly computed style with the element's old
one by value (`style_same`) and keeps the old `AuStyle` when they are
equal; a changed style takes over the old groups whose values are equal
(`adopt_groups`). Code that compares style or group pointers (layout's
`style_diff`) then sees only what changed. A marked element whose style
is unchanged does not restyle its subtree.

## Sharing

Elements with the same parent style, tag and matched rules share one
`AuStyle`; the key includes the matched `::before`, `::after`,
`::marker` and `::placeholder` rules, and an element whose pseudo-element uses `attr()` is
not shared. Custom property maps are layered: a map holds the element's
own entries and refers to the parent's map (`AuCustomProps.base`,
chains of at most 12).

## Sheets in a document

`src/page/docview.c`, "Requests" and "Styles": each `<style>`, `<link>`
and `@import` is a `SheetSrc`.

- A `<link>` fires `load` once its sheet and every `@import` under it
  have arrived, or `error` when its own sheet fails
  (`SheetSrc.event`, fired from `au_docview_run_scripts`). Code-split
  CSS loaders wait for it: without it, GitHub's dashboard never rendered
  its Copilot box or its Top repositories list (the loader's timeout is
  120 s).

- It is parsed once, when its text arrives (`parse_sheet`); the
  resolver borrows the parsed sheets, and a change of the sheet list
  detaches and adds them again (`au_css_resolver_detach_sheets`)
  without parsing.
- The parsed sheet takes the text (`au_css_sheet_parse_take`) and keeps
  it; `src_text_get` and `src_text_is` read it back for the CSSOM and
  to notice a `<style>` that was edited.
- Once the document has loaded, the texts are packed
  (`au_docview_pack_sheets`): `au_docview_update` packs one sheet on a
  pass with nothing else to do, after five seconds in which no block
  was parsed late (`pack_when_quiet`). To pack the sheets of a document
  that is no longer updated (hidden, kept for Back), call
  `au_docview_pack_sheets` until it returns false.
- Sources with the same URL, media and text share one parsed sheet, in
  one document or across documents ("Sheets shared between documents"
  below); a sheet with `@import` rules is not shared. GitHub's home
  page links one 800 KB sheet four times and one of 300 KB six times.
- A source whose media query does not match the device is neither
  fetched nor parsed (`start_sheet`); `undefer_sheets` starts it when
  the viewport or the colour scheme changes so that it matches.
- A sheet that has been parsed stays, and the cascade evaluates its
  media query.
- A `url()` value resolves against the URL of the sheet it came from, as
  CSS Values 4 says, and a `url()` in a `style` attribute against the
  document's base. The value keeps the sheet's URL
  (`AuCssBlob.base_url`), and layout passes it to the view's
  `resolve_url` callback for backgrounds, masks, `list-style-image` and
  `content` (`AuBgLayer.base_url`, `l_resolve_url`). A sheet in
  `/themes/x/dest/style.css` that names `../images/logo.svg` fetches
  `/themes/x/images/logo.svg`.

Script reaches sheets through the CSSOM
([`javascript.md`](javascript.md), "CSSOM").

## Sheets shared between documents

To parse a sheet that other documents may share, call
`au_css_sheet_parse_shared(text, len, origin, base_url, media)` instead
of `au_css_sheet_parse_take`: when a live sheet was parsed from the same
bytes for the same base URL, media text and origin, it frees the text and
returns that sheet with an owner added. The document view does this for
every `<style>`, `<link>` and `@import` (`parse_sheet` in `docview.c`), so
two tabs of one article, a frame, a document kept for Back and a shadow
root that links a page's sheet hold one parsed sheet.

- **The key.** `parser.c` keeps a list of the sheets that can be shared
  (`SharedSheet`: the sheet, an FNV-1a hash of its text, its media
  text); a match needs the same hash, length, origin, base URL and
  media, and then the same bytes (`au_css_sheet_text_is`, which reads
  packed text a piece at a time). The base URL is the URL the document
  requested (the view does not follow a redirect's final URL for it), so
  two sheets parse alike exactly when the key matches. The list holds
  the sheets that are alive and is searched in order; a sheet leaves it
  when its last owner frees it.
- **A shared sheet is never changed.** Script that changes a sheet's
  rules (`insertRule`, `deleteRule`, `cssText`, `replace`) gets a sheet of
  its own parsed from the new text (`SheetSrc.om_sheet`,
  `au_dv_sheet_set_text`); `disabled` is the source's
  (`SheetSrc.disabled`). A sheet with `@import` rules is not listed,
  since its owner attaches the imported sheets to it
  (`au_css_sheet_set_import` refuses a listed sheet). What a sheet does
  late (blocks of declarations parsed on first match, `@media` blocks
  parsed when they first apply, the packed text) reads the same to
  every owner.
- **Atoms.** A listed sheet keeps the set of atoms it refers to
  (`AuCssSheet.atoms`), and a document that shares it holds them
  (`au_atom_set_hold`); a block parsed late interns for every document
  that holds the sheet (`late_begin`). [`memory.md`](memory.md), "Atoms",
  explains why.
- **What a document is charged.** `au_docview_memory_estimate` counts a
  shared sheet's memory divided by its owners.

Tests: `test_shared_sheets` in `tests/page/test_page.c` (two documents
share a link's and a `<style>`'s sheet; one changes its sheet with the
CSSOM and the other keeps the original; disabling it in one leaves the
other; the rules parsed late work after the first document is freed;
the list is empty after both; the asan build checks the frees), and
`test_atoms_freed` for the atoms.

Measured on 2026-11-16, base `49d5f6e`, host (64-bit, bundled fonts,
`AURA_MEM=1`), with the change and with sharing turned off in the same
build, two runs each. Each cell is what the second document adds over
the first opened alone: two tabs of Wikipedia's PowerPC article after
12 seconds, and GitHub's torvalds/linux beside GitHub's home page after
15 seconds.

| Second document | `src/css`, not shared | Shared | Live bytes, not shared | Shared |
|---|---|---|---|---|
| Wikipedia PowerPC, a second tab | 1,279 / 1,266 KB | 855 / 831 KB | 14,754 / 14,684 KB | 14,066 / 14,006 KB |
| GitHub torvalds/linux beside GitHub's home page | 6,493 / 6,241 KB | 2,958 / 2,420 KB | 54,289 / 52,295 KB | 48,623 / 46,932 KB |

The second Wikipedia tab costs about 430 KB less in sheets, and the
second GitHub page about 3.7 MB less: the sheets its pages share are
parsed once. The rest of what a second document costs (its tree,
layout and script heap) is its own.

## Shadow trees and scoped styles

To give the resolver a sheet of a shadow tree, call
`au_css_resolver_add_scoped_sheet(r, sheet, shadow_root)`; a sheet added
with `au_css_resolver_add_sheet` belongs to the document. The document
view does this for each `<style>` and `<link>` in a shadow tree and for
`adoptedStyleSheets`, which come after the tree's own sheets.

To restyle after a shadow tree's sheets change, call
`au_css_resolver_sheets_changed_in(r, roots, n)`: the index is built
again and only the hosts of those roots are restyled. A change to the
document's sheets still restyles everything.

These rules keep that restyle small on a page that attaches many
components at once:

- **The index is kept when it can be** (`index_reuse` in `cascade.c`).
  The resolver records the sheets the index was built from and a
  signature of each (`AuCssSheet.serial`, folded over its imports). When
  the document's sheets are the same, every shadow root's list equals a
  list the index has already, and every indexed list is still in use,
  the index stands and only the scope keys are rebuilt. A sheet whose
  text changed gets a new serial, and so does a sheet whose `@import`
  arrived.
- **The same device changes nothing.** `au_css_resolver_set_device`
  returns at once when the device is the one the resolver has. The
  document view gives the device with every new sheet list; before
  this check each new shadow root restyled the whole document.
- **The document view keeps up to 1,024 changed shadow roots** per
  pass (`DV_MAX_DIRTY_SCOPES`). Past that the whole document is
  restyled.
- **A change between the new sheet list and the restyle is targeted.**
  The invalidation calls build the waiting index first
  (`index_current`), so a class, attribute, state or child change marks
  what its records reach instead of forcing a full restyle.

To restyle a host's whole flat subtree (a slot assignment changed),
call `au_css_invalidate_subtree`.

Everything below runs only when the document has a shadow root
(`AuDocument.has_shadow`); a page without one walks the same code as
before.

- **Scopes.** The index records a scope per sheet. Shadow roots whose
  sheet lists are equal share one scope (a page with 200 copies of one
  component indexes its sheet once). A rule matches an element only
  when its scope is the element's tree; each scope is walked under an
  anonymous cascade layer of its own.
- **The style pass walks the flat tree** (`au_flat_next`): a host's
  shadow tree in place of its children, a slot's assigned nodes in
  place of its fallback. Inheritance and custom properties follow the
  flat tree. Children of a host that no slot takes get no style
  (`clear_unshown` releases what they had).
- **`:host`, `:host()`, `:host-context()`** match the host from inside
  its shadow tree. There the host is featureless: a compound must hold
  one of these to match it (`*` alone does not). A combinator steps from
  the top of the shadow tree to the host and stops there (`m_parent`).
- **`::slotted()` and `::part()`** rules are kept out of the selector
  maps (`shadow_entries`). `collect_scoped` matches `::slotted()` rules
  of the trees of the slots a node is assigned to (a chain, for slots
  slotted again) and `::part()` rules of the trees above an element's
  tree, mapping names through `exportparts` (at most 16 names a step).
- **Cascade order** (CSS Scoping, "shadow cascade order"): the key
  holds a context code above the layer. For normal declarations an
  outer tree's rule beats an inner tree's rule (`:host` and
  `::slotted()` lose to the page's rules for the same element); for
  `!important` the inner tree wins.
- **Invalidation.** `:host` and `::slotted()` dependencies are keyed
  universally; `:host-context()` and `::part()` mark descendants. A
  change to a host's children, a slot's `name`, a node's `slot`, `part`
  or `exportparts` restyles the host's flat subtree
  (`au_dv_dom_changed`, `au_dv_child_changed`).

### Cost for a page without shadow trees, 2026-11-16

`css_bench --page` on a saved Wikipedia article (2742 elements, 2
sheets), Release build on the host, best of 12 runs, milliseconds and
bytes:

| | parse | index | first style | style again | heap total |
|---|---|---|---|---|---|
| before shadow trees | 2.0 | 1.4 | 5.0 | 4.5 | 1278864 |
| after | 2.0 | 1.4 | 5.2 | 4.6 | 1278912 |

400 restyles of the article took 1.91 s before and 1.90 s after. The
first version cost 8%: `match_pseudo` and `match_simple` gained a second
caller and the compiler stopped inlining them into `match_rest`. The
shadow cases are now out of line (`match_shadow_pseudo`,
`match_on_part`, `host_compound_ok`, all `AU_NOINLINE`), and a
pseudo-class after `::part()` is matched through `match_from`, so that
each of the hot functions keeps one caller. Keep it that way when adding
a selector feature.

## Container queries

To let the resolver read container sizes, call
`au_css_resolver_set_container_size(r, fn, ctx)`; `fn` returns an
element's content-box width and height, or false when it has no laid
out box. The document view passes `au_layout_css_container_size`.

To restyle after a layout, call `au_css_resolver_containers_changed(r,
doc)`: it reads the size of every container a query or a unit read in
the last pass, marks those whose size changed by more than 0.01 px
`STYLE_DIRTY`, and returns whether any did. Then call `au_css_restyle`
and lay out again. The document view does this in `container_rounds`
([`layout.md`](layout.md), "Container query rounds").

To drop the records before a document is freed, call
`au_css_resolver_forget_containers`; the records hold a reference to
each container element.

What is supported:

- `container-type` (`normal`, `size`, `inline-size`), `container-name`
  (`none` or idents other than `and`, `or`, `not`, `default`) and the
  `container` shorthand (`name [/ type]`).
- `@container [name] condition`, with `width`, `height`, `inline-size`,
  `block-size`, `aspect-ratio` and `orientation` in range or plain form,
  combined with `and`, `or` and `not`. A condition is evaluated in three
  values: a general-enclosed term such as `style(...)` or
  `scroll-state(...)` is unknown, and an unknown result does not match.
  `@container` rules nest, and the conditions of all enclosing rules
  must hold.
- Units `cqw`, `cqh`, `cqi`, `cqb`, `cqmin`, `cqmax`. With no container
  for the axis, or one that has no box yet, a unit is the small viewport
  unit of that axis and the style gets `AU_STYLE_F_VIEWPORT_UNITS`.

How a query is evaluated:

- The rule index keeps a list of conditions (`cq_conds`; index 0 means
  none) and, parallel to the entries, the innermost condition of each
  (`entry_cq`). Without `@container` rules `entry_cq` is NULL and
  matching takes the same path as before.
- The query container is the nearest flat-tree ancestor that has the
  query's name, if any, and whose type allows every axis the condition
  reads (`CssContainerQuery.axes`). For a pseudo-element the search
  starts at its element (`cq_self`). The element's own style is not
  stored yet while its pseudo-elements are computed, so the resolver
  reads it from `cq_pend_el` and `cq_pend_style`.
- `cq_find` caches the nearest container for each start element, name
  and set of axes within one style pass (`CqNear`, 256 slots), so a walk
  up a deep document stops at the first ancestor with an answer. Each
  condition also keeps its last result per start element (siblings
  share one).
- An element with a pseudo-element rule inside `@container`
  (`cq_pseudo`), and a style that used a container unit
  (`AU_STYLE_F_CONTAINER_UNITS`), are not shared: two siblings can see
  different containers or sizes.
- `getComputedStyle` lays the document out first when the resolver has
  read a container (`au_css_resolver_has_containers`), so that the
  result reflects the container's current size.

In a condition, `em` is the container's font size, `cqw` and `cqh`
resolve against the queried container itself, and the other container
units against the viewport (`cq_len_px` in `media.c`).

### Cost for a page without container queries, 2026-11-16

The same saved Wikipedia article, Release builds on the host of base
`b469f56` and of this change, milliseconds and bytes. `css_bench
--page`, best of 12 runs; `aura-host layout-bench PAGE --bundled-only`,
best of 16 runs (full layout at 1440 pixels, and a document view
relayout from 1440 to 656):

| | parse | index | first style | style again | heap total | full layout | relayout |
|---|---|---|---|---|---|---|---|
| before container queries | 1.9 | 1.3 | 5.1 | 4.6 | 1279248 | 1.67 | 1.74 |
| after | 1.9 | 1.3 | 5.1 | 4.6 | 1279248 | 1.69 | 1.74 |

The layout difference is within the spread between runs (1.67 to 1.78
for both builds).

## How a parsed sheet is stored

A sheet keeps its source text and parses as little of it as it can.
`css_int.h` has the records; `parser.c` builds them.

- **One arena per sheet.** Rules, selector lists, selectors, components,
  media lists, layer names, condition text and parsed declaration
  arrays are allocated in `AuCssSheet.arena` (chunks that double from
  496 bytes to 16 KB) and freed with the sheet. To add something a rule
  refers to, allocate it there with `css_arena_alloc_ptr`; do not free
  it. A selector list parsed on its own (`au_css_selector_parse`) has
  its own small arena.
- **Records.** With 32-bit pointers a rule is 20 bytes, a selector 12,
  a component 8, a rule index entry 24; `CSS_SIZE_ON_32` fails the
  Mac OS 9 build if one grows. A component holds only a name or a list;
  an attribute value, An+B, a `:lang` string or a name whose case
  differs is in a `CssSelExt` (read through `css_comp_name`,
  `css_comp_name_cs`, `css_comp_value`). A selector's ancestor filter
  hashes follow its components (`css_sel_hashes`); the rule index
  keeps the first one in its entry, so most entries are rejected
  without reading the selector, and keeps the parsed block's address
  once a rule's block is parsed (`entry_decls`).
- **The text is packed once the page has loaded**
  (`au_css_sheet_pack_text`): deflated in pieces of 4 KB
  (`CSS_TEXT_PIECE`), each a stream of its own, so reading one block
  inflates the piece it is in (`css_sheet_text_at`; one inflater and
  the last piece are shared by all sheets). A text under 4 KB, one that
  does not shrink by an eighth and the user agent sheet's stay as they
  are. Read the text only through `css_sheet_text_at`,
  `au_css_sheet_text_get` and `au_css_sheet_text_is`.
- **Blocks of declarations stay text.** A style rule whose block holds
  only declarations records the block's place in the text (`RF_LAZY`)
  and parses it when the rule first matches an element
  (`css_rule_decls`, called from `collect` in `cascade.c`), with the
  code that parses a block at sheet parse time, so errors drop the same
  declarations. Read a rule's declarations only through
  `css_rule_decls`.
- **Top-level `@media` blocks stay text** until their query first
  matches the device (`css_rule_children`, called when the rule index
  is built); one for print or for another width is never parsed. Read a
  rule's children only through `css_rule_children`.
- **What is parsed with the sheet:** a block with nested rules or
  at-rules in it, an unclosed block, `@keyframes`, `@font-face`,
  `@property`, an `@media` block that holds one of those three (they
  register with the sheet when parsed) or that is nested in another
  rule, and everything in a sheet of 4 GB or more.
- **What is not kept:** the contents of an `@supports` whose condition
  is false and of `@scope` and `@starting-style` (which never apply;
  the rule itself is kept). An `@container` rule keeps its parsed query
  (`CssContainerQuery`) and its children; one with an invalid prelude
  is dropped. A selector that cannot match
  here (`SELF_NEVER`: a pseudo-element Aura does not create, a
  pseudo-class that is never true) is not indexed, so its block is
  never parsed.
- **Nothing outside the matched path reads declarations.** The
  invalidation index (`deps_build`) and the rule index read selectors
  only; presentational hints come from attributes; `@keyframes` and
  `@font-face` are parsed with the sheet. A new reader of every rule's
  declarations would parse every block: record the fact at parse time
  instead.
- **A sheet given to a resolver several times** is indexed once, at its
  last occurrence; an earlier one only names its layers
  (`index_build`). A sheet with an anonymous layer or an `@import` is
  indexed every time.
- Equal media queries of a sheet share one list (`media_list`). During
  a sheet parse equal `var()` and custom property token lists share one
  list (`css_toklist_intern_begin`) and equal declared values one blob
  (`css_blob_intern_begin`). A token list is one allocation.

To check that lazy sheets style as eager ones do, run
`test_css_invalidate --lazy tests/page/samples` (ctest `css_lazy`) or,
for a saved page, `test_css_invalidate --lazy-page PAGE.html
SHEET.css...`. `css_lazy_bodies` and `css_index_skip_repeats`
(`css_int.h`) turn the two behaviours off for such tests, and
`css_pack_at_parse` packs every text at once, so that each late parse
reads packed text. `test_page.c` (`test_packed_sheets`) does the same
through a document view.

[`memory.md`](memory.md) lists what sheets cost.

## Selector queries

`au_css_query_selector` and `au_css_selector_matches_scoped` take the
element `:scope` stands for (the element querySelector,
querySelectorAll or closest was called on); with a document or NULL,
`:scope` is the root element.

## `@font-face` descriptors

`@font-face` descriptors are generic value trees
(`au_css_font_face_descriptor`), except `unicode-range`: a COMMA_LIST
of LISTs of two integer NUMBERs, the first and last code point of each
range; an invalid `unicode-range` is dropped. The tokenizer keeps each
number as written (`css_tok_num_text`) for this, since `U+0025-00FF`
and `U+1e3` are number tokens. `font-display` is read from its generic
value (a keyword) by `webfont.c`. [`fonts.md`](fonts.md) describes the web
font loader and the block periods.

## Values parsed from text

`src/css/canvasval.c` parses a colour and a font shorthand given as
text (`au_css_parse_color_text`, `au_css_parse_font_text`) with the
same code as declarations, for the canvas ([`canvas.md`](canvas.md)).
SVG presentation attributes reach the cascade as presentational hints
(`svg_hints` in `src/css/ua.c`, see [`svg.md`](svg.md)).

## Relative colours

`rgb()`, `rgba()`, `hsl()` and `hsla()` take the relative syntax of CSS
Color 5: `rgb(from <colour> r g b / alpha)` (`parse_relative` in
`src/css/values.c`). Each channel is a channel keyword of the origin
colour (`r g b alpha`, or `h s l alpha`), a number, a percentage,
`none`, or a `calc()` over those with `+ - * /` (no parentheses;
`rel_calc`). While a colour function's arguments are parsed,
`CssParseCtx.in_color` lets a `calc()` that names a channel through as
a plain function and lets the operators through as delimiters, since
the math parser does not know channel names. The origin is resolved at
parse time, so it cannot be `currentcolor`; a `var()` in it is
substituted first as usual. The other colour functions (`lab()`,
`oklch()`, `color()`) do not take the relative syntax yet.

## Registered custom properties

An `@property` rule (`parse_property_rule` in `src/css/parser.c`) with
`syntax` and `inherits` registers a custom property: the cascade
(`build_custom` in `src/css/cascade.c`) gives it its `initial-value`
where nothing sets it, and does not inherit it when `inherits` is
false. `CSS.registerProperty()` (`registerProperty` in
`src/js/cssom.js`) writes the same rules into a constructed sheet of
Aura's own, which the document adopts ahead of the page's adopted
sheets and which `document.adoptedStyleSheets` does not show. The
syntax is not checked against values and registered values are not
typed, so a registered `<length>` or `<color>` does not interpolate in
a transition or animation.

## Properties parsed only

These are parsed, cascaded and computed, so `CSS.supports` and
`@supports` answer true and the declarations are not dropped, but
layout and painting do not act on them yet: `accent-color` and
`caret-color` (`auto` is stored as the colour 0), `overscroll-behavior`
and its `-x` and `-y` longhands, `scrollbar-gutter` (`auto`, `stable`;
`stable both-edges` is not parsed), `content-visibility` (laid out as
`visible`), `field-sizing` and `interpolate-size`. To make one take
effect, read its field in `AuStyleRare` or `AuStyleInhRare` (`css.h`).

## Environment variables

`env()` is substituted with `var()` (`substitute` in
`src/css/cascade.c`). The safe area insets (`safe-area-inset-*`,
`safe-area-max-inset-*`) and the virtual keyboard's insets
(`keyboard-inset-*`) are `0px`: no screen Aura draws to has a notch or
an on-screen keyboard over the page. Any other name, the `titlebar-area-*`
variables included, takes its fallback, and without one the declaration
is invalid at computed-value time. Names are case-sensitive. Sites wrap
their gutters in `max(var(--wrap), env(safe-area-inset-left))`, often in
a grid's track list, so an unresolved `env()` there drops the whole
template to `none`.

## Computed values fixed after the cascade

`css_compute_finish` in `src/css/compute.c` adjusts a few values once
every declaration has applied: display is blockified, overflow pairs
and border and outline widths are corrected, and `text-align:
match-parent` (the UA sheet's `li`) becomes the parent's value, with
`start` and `end` made `left` or `right` by the parent's direction.

## Animations

To play an animation, an element needs `animation-iteration-count:
infinite`, a duration, a play state other than paused, and `@keyframes`
that set `transform` or `opacity`. Only its first animation plays; that
covers spinners and pulsing placeholders, so a page shows it is working.

- **Sampling** (`au_css_animation_sample`, `src/css/anim.c`): the time
  minus `animation-delay`, wrapped by the duration, turned by
  `animation-direction`, gives a progress. The keyframes that declare
  the property either side of it, or the element's own value at 0% and
  100%, are interpolated through `animation-timing-function` (keywords,
  `cubic-bezier()`, `steps()`).
- **Layout** (`src/layout/animate.c`): while the stacking contexts are
  built, such a block gets an `LAnim` and becomes a stacking context.
  `l_transform_matrix` interpolates the two transform lists function by
  function when they hold the same functions (`none` takes each
  function's identity), else switches halfway; paint takes the sampled
  opacity.
- **Frames** (`au_layout_anim_tick`, called from `au_docview_animate`):
  20 a second, on the document's clock from its first frame. Each frame
  repaints the rectangle where the box was drawn and the one where it is
  now. The page passes these rectangles to the window as they come
  (`page_doc.c`); before 2026-11-17 anything invalidated during the
  page's idle slice repainted the whole window, so every frame of an
  animated GIF did.
- `tests/forms/test_forms.c` (`test_css_animation`) checks a scale at
  half its period, the repainted rectangle, and that `animation: none`
  stops it.

## Tests

- `tests/css/cases/*.txt` (one ctest per file, format in `test_css.c`).
- `test_css_api.c` (API tests).
- `test_css_invalidate.c`, which changes states, classes, ids,
  attributes and inline styles on a page with and without `:has()`
  rules and checks every computed value of every element against a full
  restyle; `--lazy` compares sheets that keep blocks as text with
  sheets parsed whole (see "How a parsed sheet is stored").

## Measured on the host

| Page | What | Before | Now |
|---|---|---|---|
| A saved Wikipedia article (9301 elements, `:has()` rules) | elements restyled by a hover move | 9301 | 2 on average (8 at most) |
| | time per hover move | 12 ms | 0.012 ms |
| The GitHub home page (20 sheets, 2.5 MB of CSS, 2157 custom properties) | `aura-host chrome` peak | 147 MB | 79 MB |
| | the parsed sheets | 17.1 MB | 10.3 MB |
| | the computed styles | 44 MB | 5.8 MB |

### Parsed sheets, 2026-11-15

`css_bench --page`, macOS heap in use, 64-bit. The GitHub home page
links 28 sheets (20 distinct, 2.5 MB of text; 2078 elements); the
Wikipedia article has 2 (220 KB; 9306 elements).

| Step | GitHub: sheets, index, styles (MB) | total | Wikipedia: total (MB) |
|---|---|---|---|
| before | 23.73, 1.94, 4.41 | 30.08 | 1.78 |
| equal sheets of a document shared | 10.30, 1.94, 4.43 | 16.67 | 1.78 |
| blocks of declarations stay text | 9.08, 2.24, 5.39 | 16.71 | 1.87 |
| arena, packed records | 6.75, 1.95, 5.36 | 14.06 | 1.59 |
| `@media` blocks stay text, dead groups dropped | 5.37, 2.17, 5.29 | 12.83 | 1.58 |
| repeated sheets indexed once, token lists one block, aliases shared | 5.37, 1.34, 3.52 | 10.22 | 1.47 |
| the texts packed once the page has loaded | 3.41, 1.34, 3.52 | 8.27 | 1.31 |

Times on a quiet host, milliseconds:

| | GitHub: parse, index, first style | sum | Wikipedia: parse, index, first style | sum |
|---|---|---|---|---|
| before | 113, 5.2, 30.2 | 148 | 3.7, 0.4, 10.0 | 14.1 |
| now | 30.5, 4.9, 21.1 | 57 | 2.4, 1.5, 11.5 | 15.4 |

The Wikipedia article matches a third of its rules and applies half
its `@media` blocks, so the text of those is tokenized twice (once
with the sheet, once when the block is parsed): 1.3 ms more in all,
where a sheet whose rules mostly do not match gains. Packing the texts
takes 17 ms for GitHub and 1.3 ms for the article, after the load.

"Styles" includes what styling parses: the blocks of the rules that
matched. The sheets figure includes the text they keep (2.56 MB and
0.22 MB; packed, 0.60 MB and 0.05 MB) and the atoms their names intern.
Packing is not part of the load: it happens later, a sheet at a time.

## Not done

See [`known-gaps.md`](known-gaps.md#css).
