/* The credits table (credits.h). Keep it in step with
 * assets/fonts/README.md and third_party/. */
#include "credits.h"

const AuCredit au_credits[] = {
    {AU_CREDIT_FONT, "Aura Sans, Aura Serif, Aura Mono (from Liberation 2.1.5)",
     "Text in pages that ask for Arial, Helvetica, Times or Courier when the system lacks them",
     "Digitized data copyright (c) 2010 Google Corporation, with Reserved Font Arimo, Tinos and Cousine.\n"
     "Copyright (c) 2012 Red Hat, Inc., with Reserved Font Name Liberation.",
     "SIL Open Font License 1.1", "Fonts:liberation:LICENSE"},
    {AU_CREDIT_FONT, "Inter 4.1", "Modern interface fonts (Segoe UI, Roboto, system-ui)",
     "Copyright (c) 2016 The Inter Project Authors.", "SIL Open Font License 1.1", "Fonts:inter:LICENSE.txt"},
    {AU_CREDIT_FONT, "DejaVu Sans 2.37", "Characters no other font has: Greek, Cyrillic, symbols",
     "Copyright (c) 2003 by Bitstream, Inc. DejaVu changes are in the public domain.\n"
     "Arev Fonts copyright (c) 2006 by Tavmjong Bah.",
     "Bitstream Vera Fonts licence and Arev Fonts licence", "Fonts:dejavu:LICENSE"},
    {AU_CREDIT_FONT, "Noto Emoji 3.002", "Emoji, in monochrome", "Copyright 2013 Google LLC.",
     "SIL Open Font License 1.1", "Fonts:notoemoji:OFL.txt"},
    /* Third-party libraries. Add one entry each as they join the build,
     * in the same form:
     *
     * {AU_CREDIT_LIBRARY, "BearSSL <version>", "TLS (https:)",
     *  "<copyright line from third_party/bearssl>", "MIT licence",
     *  "<where the licence text ships>"},
     */
    {AU_CREDIT_LIBRARY, "QuickJS-ng 0.18.0", "JavaScript",
     "Copyright (c) 2017-2026 Fabrice Bellard\nCopyright (c) 2017-2024 Charlie Gordon\n"
     "Copyright (c) 2023-2026 Ben Noordhuis\nCopyright (c) 2023-2026 Sa\xc3\xbal Ibarra Corret\xc3\xa9",
     "MIT licence", "Licences:QuickJS-ng LICENSE"},
};

const size_t au_ncredits = sizeof au_credits / sizeof au_credits[0];
