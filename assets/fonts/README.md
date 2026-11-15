# Aura's bundled fonts

These faces ship in the `Fonts` folder beside the Aura application. Aura
uses them when a page asks for a family the machine does not have, and to
draw characters the page's fonts lack. System fonts always come first.

Each family's folder holds its licence. The fonts are reduced to what
Aura's engine draws (TrueType outlines, cmap, metrics, the `kern` table):
hinting, OpenType layout tables (GSUB, GPOS, GDEF) and glyph names are
removed, and glyphs only GSUB could reach are dropped. The colour emoji
face keeps its colour tables (COLR, CPAL) and its GSUB ligatures.
`tools/fonts-prepare.py` rebuilds every file here from the upstream
downloads below (it needs fontTools; building Aura does not).

`fonts.idx` lists every face (path, face index, weight, style, family) so
Aura registers them without opening the files. After changing the fonts,
regenerate it with `build/host/src/platform/host/aura-host font-index
assets/fonts`; the paint tests fail when it is out of date.

| Family | Folder | Files | Bytes |
|---|---|---|---|
| Aura Sans, Aura Serif, Aura Mono (Liberation 2.1.5) | `liberation` | 10 | 1,601,324 |
| Inter 4.1 | `inter` | 4 | 499,316 |
| DejaVu Sans 2.37 | `dejavu` | 1 | 502,256 |
| Noto Emoji 3.002 | `notoemoji` | 1 | 774,144 |
| Twemoji Mozilla 0.7.0 (colour emoji) | `twemoji` | 1 | 1,171,292 |
| Total with licences and index |  |  | about 4.6 MB |

## Aura Sans, Aura Serif, Aura Mono (from Liberation)

- Source: https://github.com/liberationfonts/liberation-fonts, release
  2.1.5, `liberation-fonts-ttf-2.1.5.tar.gz`
  (https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz,
  SHA-256 7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0).
- Licence: SIL Open Font License 1.1 (`liberation/LICENSE`), with the
  Reserved Font Names Liberation, Arimo, Tinos and Cousine.
- Files: `AuraSans-{Regular,Bold,Italic,BoldItalic}.ttf` (151,412,
  151,032, 160,948, 161,368 bytes), `AuraSerif-{Regular,Bold,Italic,BoldItalic}.ttf`
  (171,564, 166,356, 174,668, 172,552), `AuraMono-{Regular,Bold}.ttf`
  (145,808, 145,616). The upstream files are 1.2 to 1.6 MB per family.
- Changes: subset to Latin, Greek, Cyrillic, punctuation and symbols
  (U+0000-052F, 1D00-1DBF, 1E00-2BFF, 2C60-2C7F, 2E00-2E7F, A720-A7FF,
  FB00-FB06, FE20-FE2F, FFF9-FFFD; Hebrew is left out), reduced as above.
  Because the OFL forbids a modified font to use a Reserved Font Name, the
  families are renamed Aura Sans, Aura Serif and Aura Mono; the copyright,
  trademark and licence records are kept. They keep Liberation's metrics,
  which match Arial, Times New Roman and Courier New. Monospaced italics
  are not shipped; Aura synthesizes them.

## Inter

- Source: https://github.com/rsms/inter, release v4.1, `Inter-4.1.zip`
  (https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip,
  SHA-256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e),
  the static instances in `extras/ttf` (font version 4.001).
- Licence: SIL Open Font License 1.1 (`inter/LICENSE.txt`), no Reserved
  Font Name.
- Files: `Inter-Regular.ttf` (122,496), `Inter-Bold.ttf` (121,724),
  `Inter-Italic.ttf` (127,660), `Inter-BoldItalic.ttf` (127,436). The
  upstream statics are about 400 KB each.
- Changes: subset to the same ranges as Aura Sans, reduced as above.
- Use: the alias target for interface fonts Mac OS 9 lacks (Segoe UI,
  Roboto, San Francisco, Open Sans, Lato and others).

## DejaVu Sans

- Source: https://github.com/dejavu-fonts/dejavu-fonts, release 2.37,
  `dejavu-fonts-ttf-2.37.tar.bz2`
  (https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2,
  SHA-256 fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7).
- Licence: Bitstream Vera Fonts licence with DejaVu's changes in the
  public domain, and the Arev fonts licence (`dejavu/LICENSE`). A modified
  font must not be named with "Bitstream" or "Vera"; "DejaVu Sans" meets
  that.
- Files: `DejaVuSans.ttf` (502,256; upstream 757,076).
- Changes: the right-to-left scripts are removed (Hebrew, Arabic, Syriac,
  N'Ko and others: Aura has no bidirectional layout yet), reduced as
  above. Bold is synthesized.
- Use: the last-resort text face: Greek, Cyrillic, extended Latin,
  Armenian, Georgian, arrows, mathematical and technical symbols, box
  drawing, block elements, dingbats, Braille. It covers more of these per
  byte than Noto Sans, which has no symbol blocks.

## Noto Emoji

- Source: https://github.com/google/fonts, `ofl/notoemoji/NotoEmoji[wght].ttf`
  at commit b979dba422e445492b0eb9951ac52ee0b4d648c3 (font version 3.002,
  SHA-256 de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551),
  and `OFL.txt` beside it. The upstream project (googlefonts/emoji-bw)
  publishes only this variable font; there are no static instances.
- Licence: SIL Open Font License 1.1 (`notoemoji/OFL.txt`), no Reserved
  Font Name.
- Files: `NotoEmoji-Regular.ttf` (774,144; the variable font is
  1,982,596).
- Changes: the wght=400 instance is taken with fontTools' instancer, then
  reduced as above. Without GSUB the ligature glyphs for ZWJ sequences,
  flags and keycaps are unreachable and dropped; Aura draws the
  components.
- Use: monochrome emoji and pictographs: characters asked for in text
  presentation (U+FE0E, and pictographs that default to it), and emoji
  newer than Twemoji Mozilla's (Emoji 14.0). It stays behind the colour
  face in the emoji fallback list.

## Twemoji Mozilla (colour emoji)

- Source: https://github.com/mozilla/twemoji-colr, release v0.7.0,
  `Twemoji.Mozilla.ttf`
  (https://github.com/mozilla/twemoji-colr/releases/download/v0.7.0/Twemoji.Mozilla.ttf,
  1,474,284 bytes, SHA-256
  6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e), and
  `LICENSE.md` of the same tag (SHA-256
  64419edc28e9163204c3be73f835a8dfc34cd6c9b8f7d067bb685f119f839a99).
- Licence: the emoji graphics are Twemoji, copyright Twitter, Inc and
  other contributors, under Creative Commons Attribution 4.0
  International (CC BY 4.0); the font build is copyright Mozilla
  Foundation under the Apache License 2.0. `twemoji/LICENSE.md` holds
  both; `twemoji/ATTRIBUTION.txt` gives the credit, the licence link and
  the changes, as CC BY 4.0 requires. Keep both files with the font.
- Files: `TwemojiMozilla.ttf` (1,171,292).
- Changes: hinting, glyph names and the FFTM, gasp and GDEF tables are
  removed; the COLR (version 0) and CPAL tables and the `ccmp` ligatures
  of GSUB are kept. The graphics are unchanged.
- Coverage: 1,418 code points (Emoji 14.0), with ZWJ sequences, flags,
  keycaps, skin tones and tag sequences as GSUB ligatures.
- Use: the first emoji fallback face: emoji presentation (by default or
  with U+FE0F), and the `emoji` generic family.
- Chosen over Noto Color Emoji's COLR version 1 build
  (googlefonts/noto-emoji `Noto-COLRv1.ttf`, OFL): reduced the same way
  that font is 4,966,972 bytes against 1,171,292; its gradients take about
  2.6 times as long to rasterize as Twemoji's flat layers (host: 0.087 ms
  against 0.033 ms per 32 px glyph); it covers 1,508 code points (newer
  emoji) against 1,418. On a 1 GHz G4 with little memory, size and drawing
  cost decide it.

## Considered and left out

- Gelasio (a Georgia metric match): Mac OS 9.2.2 ships Georgia, and Aura
  Serif stands in where it is missing, so four more styles are not worth
  their bytes. The alias table already names Gelasio for machines that
  have it.
- Noto Sans as the last-resort face: no arrows, mathematical operators,
  box drawing or dingbats, which DejaVu Sans has.
- Liberation unmodified: 4.4 MB for the three families, and the OFL's
  Reserved Font Name rules out subsetting it under its own name.
- CJK: too large to bundle. Noto Sans CJK and Source Han Sans are CFF
  fonts, which the engine does not draw; a pack needs TrueType outlines,
  such as a static instance of google/fonts' Noto Sans SC, JP, KR or TC
  (variable glyf fonts of several megabytes each, still megabytes after
  subsetting to the common characters). A pack is a folder here per font
  with its lines in `fonts.idx` and its family in the text fallback list
  in `src/paint/fontalias.c`. Aura already tries the system's CJK faces
  (Osaka and others) when a machine has them.
