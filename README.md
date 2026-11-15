# Aura

A web browser for Mac OS 9 on PowerPC, with its engine written from
scratch in C: HTML parser, CSS, layout, rasterizer and networking. It uses
two third-party libraries, QuickJS-ng for JavaScript and BearSSL for TLS.

Aura is in early development and does not browse the web yet.

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
`Fonts` folder. Keep the two together: Aura draws pages with your
system's fonts first and uses these open source fonts (Liberation-based
Aura Sans, Serif and Mono, Inter, DejaVu Sans and Noto Emoji) for
families and characters the system lacks. Each font's licence is in its
folder; `assets/fonts/README.md` lists their sources.
