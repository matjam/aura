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
| Noto Sans SC and Noto Sans KR 2.004 (CJK) | `notosanscjk` | 2 | 3,950,712 |
| Total with licences and index |  |  | about 8.5 MB |

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

## Noto Sans SC and Noto Sans KR (Chinese, Japanese, Korean)

- Source: https://github.com/google/fonts at commit
  cc39c4c2c304bc368c9b7c311ea2bcc6013299fe, `ofl/notosanssc/NotoSansSC[wght].ttf`
  (17,772,300 bytes, SHA-256
  a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da) and
  `ofl/notosanskr/NotoSansKR[wght].ttf` (10,414,588 bytes, SHA-256
  194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252), font
  version 2.004, and `ofl/notosanssc/OFL.txt` (SHA-256
  1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9). These
  are variable fonts with TrueType outlines; the Noto Sans CJK and Source
  Han Sans releases are CFF, which Aura does not draw. To fetch them, use
  `https://raw.githubusercontent.com/google/fonts/COMMIT/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf`
  (and the same for `notosanskr`), then save them in SRC as
  `NotoSansSC[wght].ttf`, `NotoSansKR[wght].ttf` and `NotoSansCJK-OFL.txt`
  for `tools/fonts-prepare.py --only=cjk`.
- Licence: SIL Open Font License 1.1 (`notosanscjk/OFL.txt`), copyright
  Adobe, with the Reserved Font Name "Source". The Noto names are not
  reserved, so the subsets keep them.
- Files: `NotoSansSC-Regular.ttf` (3,517,076; 10,864 characters) and
  `NotoSansKR-Regular.ttf` (433,636; 2,506 characters).
- Changes: the wght=400 instance is taken with fontTools' instancer,
  subset, and reduced as above (vertical metrics, VORG and BASE go too).
  Bold is synthesized.
  - Noto Sans SC keeps the ideographs of GB 2312 (6,763, simplified
    Chinese), the Big5 common set (level 1, 5,401, traditional Chinese)
    and the JIS X 0208 kanji: 10,269 together, of which it has every one.
    It also keeps kana, bopomofo, the CJK punctuation, and the characters
    of those standards and KS X 1001 in the CJK blocks (U+2E80-33FF,
    vertical, compatibility and small forms, fullwidth forms), less
    the Korean letters. Its glyphs are ordered with the ideographs common
    to GB 2312 level 1, JIS X 0208 level 1 and Big5 level 1 first, because
    Aura reads this face in blocks (see `docs/fonts.md`, "Paged faces").
  - Noto Sans KR keeps the 2,350 hangul syllables of KS X 1001, the
    compatibility jamo, the circled hangul, and the KS X 1001 characters
    in the CJK blocks that Noto Sans SC lacks. Korean pages take their
    hanja from Noto Sans SC.
- Use: the last faces of the text fallback list, after the system's CJK
  faces (Osaka and others).
- Han unification lets one face draw the ideographs of all three
  languages, in that face's regional shapes: Japanese and traditional
  Chinese text gets the simplified Chinese design for the characters
  whose shapes differ by region. Measured on 2026-11-15, with the same
  character sets:

  | Choice | Bytes | Deflated (gzip -9) |
  |---|---|---|
  | Noto Sans SC (all ideographs, kana) and Noto Sans KR (hangul), shipped | 3,950,712 | 2,469,567 |
  | Separate SC, TC, JP and KR faces, each with its own standard | 6,945,536 | 4,237,435 |
  | One face from Noto Sans JP, without hangul | 2,804,024 | not measured |

  The separate faces cost 3 MB more for regional shapes, and choosing
  among them needs the element's language, which the fallback code does
  not see. Noto Sans JP lacks 2,215 of the GB 2312 ideographs and 122 of
  the Big5 common set; Noto Sans SC has all of them. Big5 level 2 (7,652
  rarer characters) would add about 2.6 MB, and the other 8,822 modern
  hangul syllables about 2 MB.

## Considered and left out

- Gelasio (a Georgia metric match): Mac OS 9.2.2 ships Georgia, and Aura
  Serif stands in where it is missing, so four more styles are not worth
  their bytes. The alias table already names Gelasio for machines that
  have it.
- Noto Sans as the last-resort face: no arrows, mathematical operators,
  box drawing or dingbats, which DejaVu Sans has.
- Liberation unmodified: 4.4 MB for the three families, and the OFL's
  Reserved Font Name rules out subsetting it under its own name.
- Noto Sans CJK and Source Han Sans: CFF fonts, which the engine does
  not draw.
