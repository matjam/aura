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
  (`fontdir.c`), lazily.

## The bundled set

The bundled set is in `assets/fonts`, one folder per family with its
licence, about 4.6 MB:

| Family | Notes |
|---|---|
| Aura Sans, Aura Serif, Aura Mono | Liberation 2.1.5, metric-compatible with Arial, Times New Roman and Courier New, renamed because Liberation's Reserved Font Name forbids a subset under its own name |
| Inter | modern interface fonts |
| DejaVu Sans | the last-resort text face |
| Noto Emoji | monochrome emoji |
| Twemoji Mozilla | colour emoji, COLR version 0 |

`assets/fonts/README.md` records sources, versions, licences, sizes,
the choice of Twemoji over Noto Color Emoji, and what was left out.

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
  with wide coverage, including CJK faces when a machine has them), the
  emoji list, then any face already loaded.
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
  (font-display: swap). The faces go with the document.
- `tests/page/samples/webfonts.html` shows a variable WOFF2 font at
  several weights and a WOFF face limited by `unicode-range`.

[`css.md`](css.md) describes the parsed `@font-face` descriptors.

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
