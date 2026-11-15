/* The credits table (credits.h). Keep it in step with
 * assets/fonts/README.md and third_party/. */
#include "credits.h"

const AuCredit au_credits[] = {
    {AU_CREDIT_FONT, "Aura Sans, Aura Serif, Aura Mono (from Liberation 2.1.5)",
     "Text in pages that ask for Arial, Helvetica, Times or Courier when the system lacks them",
     "Digitized data copyright (c) 2010 Google Corporation, with Reserved Font Arimo, Tinos and Cousine.\n"
     "Copyright (c) 2012 Red Hat, Inc., with Reserved Font Name Liberation.",
     "SIL Open Font License 1.1", "Fonts:liberation:LICENSE", NULL},
    {AU_CREDIT_FONT, "Inter 4.1", "Modern interface fonts (Segoe UI, Roboto, system-ui)",
     "Copyright (c) 2016 The Inter Project Authors.", "SIL Open Font License 1.1", "Fonts:inter:LICENSE.txt", NULL},
    {AU_CREDIT_FONT, "DejaVu Sans 2.37", "Characters no other font has: Greek, Cyrillic, symbols",
     "Copyright (c) 2003 by Bitstream, Inc. DejaVu changes are in the public domain.\n"
     "Arev Fonts copyright (c) 2006 by Tavmjong Bah.",
     "Bitstream Vera Fonts licence and Arev Fonts licence", "Fonts:dejavu:LICENSE", NULL},
    {AU_CREDIT_FONT, "Noto Emoji 3.002", "Emoji, in monochrome", "Copyright 2013 Google LLC.",
     "SIL Open Font License 1.1", "Fonts:notoemoji:OFL.txt", NULL},
    {AU_CREDIT_FONT, "Twemoji Mozilla 0.7.0", "Emoji, in colour",
     "Emoji graphics: Twemoji, copyright 2020 Twitter, Inc and other contributors (CC BY 4.0, "
     "https://github.com/twitter/twemoji).\n"
     "Font build: copyright Mozilla Foundation (Apache License 2.0, "
     "https://github.com/mozilla/twemoji-colr).\n"
     "Changes for Aura: hinting, glyph names and some tables removed; the graphics are unchanged.",
     "Creative Commons Attribution 4.0 International (graphics) and Apache License 2.0 (font build)",
     "Fonts:twemoji:LICENSE.md", NULL},
    /* Third-party libraries. Add one entry each as they join the build:
     *
     * {AU_CREDIT_LIBRARY, "QuickJS-ng <version>", "JavaScript",
     *  "<copyright line from third_party/quickjs-ng>", "MIT licence",
     *  "<where the licence text ships>", NULL},
     */
    {AU_CREDIT_LIBRARY, "BearSSL 0.6", "TLS for https: connections", "Copyright (c) 2016 Thomas Pornin.",
     "MIT licence", "Licences:BearSSL.txt", NULL},
    {AU_CREDIT_DATA, "Mozilla's root certificates", "The certificate authorities https: trusts (the aura-roots file)",
     "Mozilla's root store (NSS certdata.txt) as published by the curl project, https://curl.se/ca/cacert.pem.",
     "Mozilla Public License 2.0", NULL,
     "This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL "
     "was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.\n"
     "Aura carries the certificates in its own binary form. The source form is available from "
     "https://hg.mozilla.org/projects/nss (lib/ckfw/builtins/certdata.txt) and https://curl.se/ca/cacert.pem."},
    {AU_CREDIT_DATA, "The Public Suffix List", "Which domains may share cookies, and the sites in the cookie manager",
     "Maintained by the Public Suffix List community, https://publicsuffix.org/.", "Mozilla Public License 2.0",
     NULL,
     "This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL "
     "was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.\n"
     "Aura carries the list compiled into a table. The source form is available from "
     "https://publicsuffix.org/list/public_suffix_list.dat."},
    {AU_CREDIT_DATA, "The Brotli dictionary (libbrotlicommon 1.2.0)", "Decoding Brotli-compressed pages",
     "Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors.", "MIT licence", NULL,
     "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and "
     "associated documentation files (the \"Software\"), to deal in the Software without restriction, including "
     "without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
     "copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the "
     "following conditions:\n"
     "The above copyright notice and this permission notice shall be included in all copies or substantial "
     "portions of the Software.\n"
     "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT "
     "LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO "
     "EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER "
     "IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE "
     "USE OR OTHER DEALINGS IN THE SOFTWARE."},
};

const size_t au_ncredits = sizeof au_credits / sizeof au_credits[0];
