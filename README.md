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

The Mac OS 9 build needs Retro68's PowerPC toolchain. Set `RETRO68` to its
folder, or keep `os9-protect` next to this repository.
