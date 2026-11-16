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
- Child insertions and removals are not targeted: they restyle the
  element's subtree and its following siblings' subtrees, or the
  document when a `:has()` rule exists.

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
- Container queries are not evaluated, so nothing is recorded for them.
- `test_css_invalidate --viewport DIR` (ctest `css_viewport`) resizes
  pages through fourteen sizes and compares every computed value with a
  document styled from scratch.

[`layout.md`](layout.md) says what layout reuses after the restyle.

## Equal styles keep their object

A style pass compares each newly computed style with the element's old
one by value (`style_same`) and keeps the old `AuStyle` when they are
equal; a changed style takes over the old groups whose values are equal
(`adopt_groups`). Code that compares style or group pointers (layout's
`style_diff`) then sees only what changed. A marked element whose style
is unchanged does not restyle its subtree.

## Sharing

Elements with the same parent style, tag and matched rules share one
`AuStyle`; the key includes the matched `::before`, `::after` and
`::marker` rules, and an element whose pseudo-element uses `attr()` is
not shared. Custom property maps are layered: a map holds the element's
own entries and refers to the parent's map (`AuCustomProps.base`,
chains of at most 12).

## Sheets in a document

`src/page/docview.c`, "Requests" and "Styles": each `<style>`, `<link>`
and `@import` is a `SheetSrc`.

- It is parsed once, when its text arrives (`parse_sheet`); the
  resolver borrows the parsed sheets, and a change of the sheet list
  detaches and adds them again (`au_css_resolver_detach_sheets`)
  without parsing.
- A fetched sheet's text is freed after parsing; a `<style>`'s is kept
  to notice edits.
- A source whose media query does not match the device is neither
  fetched nor parsed (`start_sheet`); `undefer_sheets` starts it when
  the viewport changes so that it matches.
- A sheet that has been parsed stays, and the cascade evaluates its
  media query.

Script reaches sheets through the CSSOM
([`javascript.md`](javascript.md), "CSSOM").

**Memory of parsed sheets.** During a sheet parse equal `var()` and
custom property token lists share one list
(`css_toklist_intern_begin`) and equal declared values one blob
(`css_blob_intern_begin`); arrays are trimmed to their length.
[`memory.md`](memory.md) lists what sheets still cost.

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
and `U+1e3` are number tokens. [`fonts.md`](fonts.md) describes the web
font loader.

## Values parsed from text

`src/css/canvasval.c` parses a colour and a font shorthand given as
text (`au_css_parse_color_text`, `au_css_parse_font_text`) with the
same code as declarations, for the canvas ([`canvas.md`](canvas.md)).
SVG presentation attributes reach the cascade as presentational hints
(`svg_hints` in `src/css/ua.c`, see [`svg.md`](svg.md)).

## Tests

- `tests/css/cases/*.txt` (one ctest per file, format in `test_css.c`).
- `test_css_api.c` (API tests).
- `test_css_invalidate.c`, which changes states, classes, ids,
  attributes and inline styles on a page with and without `:has()`
  rules and checks every computed value of every element against a full
  restyle.

## Measured on the host

| Page | What | Before | Now |
|---|---|---|---|
| A saved Wikipedia article (9301 elements, `:has()` rules) | elements restyled by a hover move | 9301 | 2 on average (8 at most) |
| | time per hover move | 12 ms | 0.012 ms |
| The GitHub home page (20 sheets, 2.5 MB of CSS, 2157 custom properties) | `aura-host chrome` peak | 147 MB | 79 MB |
| | the parsed sheets | 17.1 MB | 10.3 MB |
| | the computed styles | 44 MB | 5.8 MB |

## Not done

See [`known-gaps.md`](known-gaps.md#css).
