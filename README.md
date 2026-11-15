# Aura

A web browser for Mac OS 9 on PowerPC, with its engine written from
scratch in C: HTML parser, CSS, layout, rasterizer and networking. It uses
two third-party libraries, QuickJS-ng for JavaScript and BearSSL for TLS.

Aura is in early development. It loads web pages over HTTP and HTTPS
(TLS 1.2), keeps cookies, and downloads files; it does not run
JavaScript or show every image format yet.

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
authorities HTTPS trusts (Mozilla's root set). Keep them together; without
`aura-roots` no HTTPS site loads. Aura needs Open Transport with TCP/IP
configured. In the fonts folder: Aura draws pages with your
system's fonts first and uses these open source fonts (Liberation-based
Aura Sans, Serif and Mono, Inter, DejaVu Sans, Noto Emoji and the colour
emoji font Twemoji Mozilla) for families and characters the system lacks.
Each font's licence is in its folder; `assets/fonts/README.md` lists their
sources. Pages can also bring their own fonts with `@font-face` (WOFF2,
WOFF or TrueType, variable fonts included).
