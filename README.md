# Aura

[![CI](https://github.com/matjam/aura/actions/workflows/ci.yml/badge.svg)](https://github.com/matjam/aura/actions/workflows/ci.yml)

A web browser for Mac OS 9 on PowerPC, with its engine written from
scratch in C: HTML parser, CSS, layout, rasterizer and networking. It uses
two third-party libraries, QuickJS-ng for JavaScript and Mbed TLS for TLS.

Aura is in early development. It loads web pages over HTTP and HTTPS
(TLS 1.3 and 1.2), runs JavaScript, keeps cookies, blocks trackers with
downloaded blocklists, and downloads files. Many sites do not work yet.

## Download

Releases are on the [releases page](https://github.com/matjam/aura/releases).
Each has one self-extracting archive in two forms:

- `Aura-VERSION.hqx`, BinHex text, which survives any transfer. Decode it
  on the Mac with StuffIt Expander (part of Mac OS 9).
- `Aura-VERSION.sea.bin`, MacBinary, for tools that keep resource forks.

Either gives the application "Aura VERSION Installer". Open it on Mac OS
9.1 to 9.2.2 and choose where the Aura folder goes. The folder holds
Aura, its fonts, the root certificates and the licences (Aura's own is
MIT, in `Licences`). The build of every
commit is attached to its
[CI run](https://github.com/matjam/aura/actions/workflows/ci.yml) in the
same two forms.

## Build

To build and test on the development machine, then build the Mac OS 9
application, run:

```bash
./build.sh
```

The Mac OS 9 build needs Retro68's PowerPC toolchain. To install it in
`~/.local/opt/retro68`, run `toolchain/build.sh`. To use one you already
have, set `RETRO68` to its folder.

The build leaves the application in `build/ppc/dist/Aura`, beside a
`Fonts` folder, a `Licences` folder and `aura-roots`, the certificate
authorities HTTPS trusts (Mozilla's root set), and packs that folder as
`build/ppc/dist/Aura-VERSION.hqx` and `Aura-VERSION.sea.bin`. Keep the
folder's files together; without `aura-roots` no HTTPS site loads. Aura needs Open Transport with TCP/IP
configured. In the fonts folder: Aura draws pages with your
system's fonts first and uses these open source fonts (Liberation-based
Aura Sans, Serif and Mono, Inter, DejaVu Sans, Noto Emoji and the colour
emoji font Twemoji Mozilla) for families and characters the system lacks.
Each font's licence is in its folder; `assets/fonts/README.md` lists their
sources. Pages can also bring their own fonts with `@font-face` (WOFF2,
WOFF or TrueType, variable fonts included).

## Licence

Aura's own code is under the MIT licence; see `LICENSE`. QuickJS-ng,
Mbed TLS (Apache-2.0), the bundled fonts, the root certificates and the Public Suffix
List keep their own licences: see `third_party/README.md`,
`assets/fonts/README.md` and Aura's About box.
