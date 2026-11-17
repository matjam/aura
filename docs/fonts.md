# Fonts

This document covers where Aura's faces come from, the bundled set, the
alias table, per-character fallback, variable fonts, colour glyphs,
GSUB, web fonts and the test fonts. Read it before changing anything in
`src/paint` that touches fonts, `src/page/webfont.c`, or
`assets/fonts`. How layout rounds font metrics and paints text is in
[`layout.md`](layout.md).

Aura draws with the system's fonts first and with the fonts bundled in
the `Fonts` folder beside the application for families and characters
the system lacks.

## To add a font

1. Put its folder (licence included) in `assets/fonts`.
2. Reduce it with `tools/fonts-prepare.py` (one family:
   `--only=FAMILY`) or pyftsubset to TrueType outlines (no CFF),
   keeping file and folder names to 31 characters.
3. Regenerate the index with `build/host/src/platform/host/aura-host
   font-index assets/fonts`.
4. Add its sources to the README.
5. Name it in the alias table or the fallback lists if it should stand
   in for other families.
6. Add it to the credits table
   ([`browser-chrome.md`](browser-chrome.md)).

The paint tests check that `fonts.idx` matches the folder and that
every face parses and draws.

## To see which fonts Aura uses

- On the host, `aura-host fonts` lists the faces and marks the bundled
  ones.
- `aura-host` takes `--bundled-only` (no system fonts: reproducible
  renders, close to a Mac OS 9 machine without the system's fonts),
  `--no-bundled-fonts` and `--fonts-dir D`; `$AURA_FONTS` also names
  the folder.
- In the guest, Develop > Tests > Font List (about:fonts) shows them
  ([`developer-tools.md`](developer-tools.md)).

## Where faces come from

- **Mac OS 9.** `platform_mac.c` registers every outline face of the
  FONDs in the resource chain (their `sfnt` resources load on first
  use), then the bundled faces listed in `Fonts/fonts.idx`.
- **Host.** `platform_host.c` loads a fixed list of macOS fonts that
  match Mac OS 9's, then the bundled faces of `assets/fonts`
  (`src/platform/posix/fontdir_posix.c`), lazily.
- Both platforms register a bundled face with `au_fontset_add_file`. A
  file over 2 MB (`AU_FONT_PAGED_MIN`) becomes a paged face (below);
  a smaller one is read whole, on Mac OS 9 into temporary memory when
  there is room.

## Paged faces

A paged face keeps its glyph outlines in the file (`src/paint/fontpage.c`).
On first use the pager reads the table directory and every table but
`glyf` into one compact sfnt in memory (cmap, metrics, loca, names), and
reads `glyf` in 16 KB blocks (`AU_FONT_PAGE_SIZE`) as glyphs are drawn,
keeping the 4 most recently used blocks (`AU_FONT_PAGE_SLOTS`) and a copy
of each glyph read, up to 256 KB of them (`AU_FONT_GLYPH_CACHE`), least
recently used out first. A glyph drawn again, at another size or after
the glyph cache dropped it, reads nothing; a block is read for each
glyph whose block is not among the 4. Noto Sans SC is the only bundled
face this large.

- To see what paged faces hold, run `aura-host render PAGE.html -w 1024
  -h 768 -o run/a.png --bundled-only`: the last lines give the font data
  loaded and the blocks read. In the application, Develop > System
  Status shows "Paged faces" with the blocks read.
- The platform supplies `size` and `read` callbacks (`AuFontFileOps`);
  on Mac OS 9 the size comes from the catalog and the data fork stays
  open for reads (`platform_mac.c`), on the host a `FILE`
  (`fontdir_posix.c`).
- A read that fails, or a file shorter than its tables say, gives
  missing glyphs. The pager checks every table range against the file
  length, at most 256 tables and 16 MB of tables in memory, and a glyph
  of at most 256 KB.
- Glyphs are read through `glyph_get` in `sfnt.c`, which returns the
  glyph's bytes from the face's data or the pager; the outline readers
  check each read against the glyph's length.
- `tools/fonts-prepare.py` orders Noto Sans SC's glyphs with the most
  common ideographs first, so a page's characters share fewer blocks.

Measured on the host (Apple silicon, 2026-11-15, `--bundled-only`,
1024 x 768):

| What | Without the CJK faces | With them |
|---|---|---|
| Live memory after loading the Japanese Wikipedia front page, first screen painted (`AURA_MEM=1`) | 11,514 KB | 12,262 KB |
| Of which Noto Sans SC: tables, blocks, 76 glyphs kept | | 117 KB, 64 KB, 18 KB |
| Noto Sans SC if it were read whole | | 3,435 KB |
| Blocks read to paint the first screen (1024 x 768) of the Japanese, Chinese front page | | 45, 76 |
| Blocks read to paint the whole Japanese front page (7588 px, 685 distinct CJK characters) | | 573 |
| Blocks read to paint the whole Chinese front page (12,396 px, 771 characters) | | 725 |
| The first CJK glyph: fallback, opening the face, reading and drawing the glyph (`test_fallback`, warm file cache) | | 0.1 to 0.4 ms |

Simulated from the glyph offsets, the 685 distinct CJK characters of the
Japanese front page lie in 339 of the face's 4 KB blocks in Unicode
order, 252 with the common characters first, and 101 16 KB blocks. A
whole page reads about one block per distinct character because the
characters arrive in text order; keeping 16 blocks instead of 4 read
561 instead of 573, so the blocks are not worth their memory, and the
repeated reads of a block come from the Mac OS disk cache.

On a 1 GHz G4 (estimated, not measured): the work in memory is about
2 to 8 ms (twenty times the host, the ratio the QEMU guest showed for
deflate in [`memory.md`](memory.md)); a disk of the time adds about
20 ms to read the catalog entry and 120 KB of tables, and about 10 ms
for a block not in the Mac OS disk cache. The first CJK glyph should
appear 30 to 40 ms after the page needs it. A first screen reads 45 to
76 blocks: with the file in the disk cache that is about 0.1 ms each,
and cold at most the 15 to 30 distinct blocks seek, 0.2 to 0.3 s.
Reading the whole face at once would take about 0.25 s and 3.4 MB.

## The bundled set

The bundled set is in `assets/fonts`, one folder per family with its
licence, about 8.5 MB:

| Family | Notes |
|---|---|
| Aura Sans, Aura Serif, Aura Mono | Liberation 2.1.5, metric-compatible with Arial, Times New Roman and Courier New, renamed because Liberation's Reserved Font Name forbids a subset under its own name |
| Inter | modern interface fonts |
| DejaVu Sans | the last-resort text face |
| Noto Emoji | monochrome emoji |
| Twemoji Mozilla | colour emoji, COLR version 0 |
| Noto Sans SC | the ideographs of Chinese, Japanese and Korean (GB 2312, Big5 level 1, JIS X 0208), kana, bopomofo, CJK punctuation; paged |
| Noto Sans KR | hangul (the 2,350 syllables of KS X 1001) |

`assets/fonts/README.md` records sources, versions, licences, sizes,
the choice of Twemoji over Noto Color Emoji, the choice of one face for
the ideographs over one face per language, and what was left out.

## The alias table

The alias table is `src/paint/fontalias.c`: rows map a family name as
pages write it (ASCII case-insensitive) to the families tried in its
place, installed system families first and bundled faces last. Rows for
the CSS generic families also name the preference slot whose family
(the user's font preferences, or the platform's choice) is tried before
the row; a target naming a generic expands to it. To extend it, add a
row.

Matching (`au_fontset_match`) tries the families the page names up to
its first generic family, then their alias targets, then the generic,
and so on, then sans-serif. So "Roboto, Arial, sans-serif" uses an
installed Arial before Inter, and "Roboto, sans-serif" uses Inter.

## Per-character fallback

Per-character fallback is `AuFontList` (`au_fontlist_new`) and
`au_shape_text_list` in `src/paint/fallback.c`, which layout uses for
all text through its `LFont`.

- **Clusters.** Text is split into clusters (a base with its combining
  marks, variation selectors and emoji modifiers) and each cluster gets
  the first face that has all of it.
- **Order.** The primary face, the list's other candidates in matching
  order, the text fallback list (DejaVu Sans first, then system faces
  with wide coverage, including CJK faces when a machine has them, then
  the bundled Noto Sans SC and Noto Sans KR), the emoji list, then any
  face already loaded.
- **CJK.** One face draws the ideographs of all three languages (Han
  unification), so the fallback order is fixed and does not depend on
  the element's `lang`. Japanese and traditional Chinese text gets Noto
  Sans SC's shapes for the characters whose design differs by region.
- **Emoji presentation** (by default or with U+FE0F) tries a colour
  font the page names, then the emoji list (Twemoji Mozilla first);
  text presentation (U+FE0E) takes the monochrome Noto Emoji.
- **Sequences.** An emoji sequence (ZWJ sequences, flags, keycaps, skin
  tones, tag sequences) is one cluster: the face's GSUB ligature draws
  it, and without one its components are drawn.
- Default-ignorable code points draw nothing.
- Marks are placed over their base by their ink boxes (the fonts carry
  no GPOS for Aura to read).
- Fallback faces keep the requested weight and style, synthesized when
  missing.
- A per-list cache and a per-set cache keyed by code point and style
  keep this fast; a face is loaded only when it is tried.

**Line height.** With `line-height: normal`, a line holding fallback
glyphs grows to the fallback face's ascent and descent (each with half
its line gap) where they exceed the primary's, as Blink does; a fixed
line height ignores fallback faces. Layout rounds these metrics to
whole pixels (see [`layout.md`](layout.md)).

## Variable fonts

`fontvar.c`: fvar, avar (versions 1 and 2), gvar (with IUP, composite
offsets and phantom points), HVAR, MVAR and STAT names.

- `au_font_face_instance` makes an instance at axis values.
- `AuFontSet` matching makes instances for the requested weight (wght),
  italic (ital, else slnt), and, through `au_fontlist_new_style`,
  stretch (wdth), optical size (opsz) and `font-variation-settings`.
- Instances are cached per set entry (at most 64).
- A weight the font cannot reach is synthesized.
- Glyph boxes (`au_font_glyph_box`) are the default instance's.

## Colour glyphs

`colr.c`: CPAL palette 0, COLR version 0 layers and version 1 paint
graphs (solid, linear, radial and sweep gradients, transforms,
composite modes), with limits on depth (32), paints visited (4096) and
cycles. Colour glyphs go in the glyph cache as premultiplied pixels
under the same byte limit, keyed with the text colour only when they
use it.

## GSUB

`gsub.c`: lookup types 1 (single), 4 (ligature) and 7 (extension) of
the features ccmp, locl, rlig, liga and clig, for the DFLT, else latn,
script. Fallback uses it for emoji sequences only; Latin text is not
shaped.

## Web fonts

- `au_font_decode` (`woff.c`) turns WOFF 1.0 and WOFF2 (transformed
  glyf, loca and hmtx; collections) into sfnt bytes and refuses
  CFF-flavoured fonts.
- `src/page/webfont.c` loads `@font-face` rules for a document view
  into a font set layered over the platform set
  (`au_fontset_new_layered`, `au_fontset_add_web`), fetching only faces
  whose family the styled document uses for text in their
  `unicode-range`, and lays the document out again when one arrives
  and is used (see [Block periods](#block-periods-and-font-display)).
  The faces go with the document.
- The text a face is checked against is each text node and the
  `content` of each `::before` and `::after` (`scan_pseudo`), since
  icon fonts are often used only there. A string is checked as written;
  a counter, `attr()` or quote is checked as digits, Latin letters and
  quote marks, because its text is not known until layout.
- `tests/page/samples/webfonts.html` shows a variable WOFF2 font at
  several weights and a WOFF face limited by `unicode-range`.

[`css.md`](css.md) describes the parsed `@font-face` descriptors.

### Block periods and font-display

Text in a face that is still loading is laid out with the fallback's
metrics and painted invisible (text and its decorations) for the face's
block period, as Chrome does, so a face that arrives in time costs one
layout and no visible re-flow. The period starts when the face is first
fetched. When it ends the fallback shows with a repaint only, and the
face swaps in later only as `font-display` allows:

| `font-display` | Block period | Face used if it arrives |
|---|---|---|
| `auto` (and an invalid value), `block` | 3 s | at any time |
| `swap` | 100 ms | at any time |
| `fallback` | 100 ms | within 3 s after the block period |
| `optional` | 100 ms | within the block period |

- To change the periods, edit `AU_WEBFONT_BLOCK_MS`,
  `AU_WEBFONT_SHORT_BLOCK_MS` and `AU_WEBFONT_SWAP_MS` in
  `src/page/webfont.h`. There is no preference.
- A face whose fetch fails (all its sources) ends its block period at
  once. A face not used is dropped undecoded.
- Text is hidden only while the first family in its list that the page
  can use with U+0020 is a blocked web face; a platform or generic
  family ahead of it shows at once, and a page without `@font-face`
  rules paints as before.
- The clock is the document view's `now_ms` callback
  (`au_webfonts_set_clock`); without one nothing is hidden and faces
  swap in when they arrive (as in the host tests that give none).
  Expiry is noticed in
  `au_docview_animate`, which a page calls on idle while fetches are
  outstanding, so no platform timer is needed.
- On a slow link (Mac OS 9 on a modem or a congested network) most
  faces miss the 3 s period: text in them appears in the fallback after
  3 s and the page re-flows once per face when it arrives, and a
  `fallback` or `optional` face usually keeps the fallback. A page never
  stays blank past the period, whatever the network does.
- Layout asks through the env callback `text_hidden`; the document
  view calls `au_layout_text_visibility_changed` when a period ends,
  which repaints without a new layout ([`layout.md`](layout.md)).
- `test_font_display` in `tests/page/test_page.c` covers each value with
  a delayed fetch and a test clock.

## Test fonts

`tests/paint/fonts` holds small subsets of Inter Variable, Open Sans
variable, Twemoji Mozilla and Noto Color Emoji (COLR version 1), WOFF
and WOFF2 versions, a font with every COLR paint format, and static
instances made by fontTools' instancer as references.

- To rebuild them, run `tools/test-fonts.py fetch`, then
  `tools/test-fonts.py make` (fontTools and the brotli command needed;
  building Aura needs neither).
- `tools/test-fonts.py refs` writes full fonts for
  `paint_fontvar_full`, which skips without them.
- To fuzz the font parsers for longer, run the sanitizer build's
  `fuzz_font ITERATIONS SEED SECONDS tests/paint/fonts/*.ttf
  tests/paint/fonts/*.woff*` and `fuzz_webfont ITERATIONS SEED
  SECONDS`.
- `test_fontvar DIR --bench`, `test_colr DIR FONTS --bench` and
  `test_woff DIR --bench FILE` time varied glyphs, colour glyphs and
  decoding.

## Not done

See [`known-gaps.md`](known-gaps.md#fonts).
