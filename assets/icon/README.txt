Aura's application icon

Aura's mascot is the kookaburra. To rebuild the icon, run
tools/make-icon.py. It reads the art of the concept named by CONCEPT at
the top of the script (kookaburra) and writes
src/platform/macos9/aura_icon.r and the previews in preview/. To switch to
another concept, change CONCEPT (frill, kookaburra, platypus or quoll), or
pass -c NAME, and run it again. To rebuild the contact sheet of all
concepts (concepts.png), add --sheet.

The Finder shows a bundle icon from its desktop database, at most 256
colours. To have Icon Services draw the 32-bit icon in the Finder too, run
tools/make-icon.py --custom-icon, which also stores the 'icns' at ID
-16455, and set the built file's hasCustomIcon flag. Retro68's Rez clears
every Finder flag, so set hasBundle (and hasCustomIcon with the option)
after each build:

  tools/finder-flags.py --bundle [--custom-icon] build/ppc/src/platform/macos9/Aura.bin

To check its CRC code, run tools/finder-flags.py --self-test; to see a
file's flags, --show.

Each concept is a folder holding icon.txt. The kookaburra and frill
folders also hold the large illustration, aura.svg, and its render
aura.png (rsvg-convert -w 256 -h 256 aura.svg -o aura.png).

The icon.txt format

  # comment
  name Frill-necked lizard
  palette
  # ch rgb    alpha c4 bw
  .    -      0     -  0
  K    000000 255   15 1
  ...
  end
  icon 32
  <32 rows of 32 characters>
  end
  icon 16
  <16 rows of 16 characters>
  end
  icon 48
  <48 rows of 48 characters, optional>
  end
  bw 32
  <optional hand-drawn 1-bit icon: # black, . white>
  end

Each palette line gives a character, its 32-bit colour (hex RGB, or - for
none), its alpha (0 to 255), its index in the Mac OS 16-colour palette
(0 white, 1 yellow, 2 orange, 3 red, 4 magenta, 5 purple, 6 blue, 7 cyan,
8 green, 9 dark green, 10 brown, 11 tan, 12 light grey, 13 grey, 14 dark
grey, 15 black) and its 1-bit value: 0 white, 1 black, l light grey (1 in
4 black), m mid grey (checkerboard), d dark grey (3 in 4 black).

The 8-bit icons use the nearest colour in the Mac OS 8-bit system palette;
colours whose channels are multiples of 33 hex (00, 33, 66, 99, CC, FF)
map exactly. Pixels with alpha below 128 (a glow) appear only in the
32-bit icons; the 1-bit mask and the 8-bit, 4-bit and 1-bit icons leave
them out. Without a bw grid the 1-bit icon comes from the palette's bw
column, with black forced on every pixel at the mask's edge.

Light comes from the upper left. Design each size as pixel art directly;
do not shrink a larger size.
