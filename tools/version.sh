#!/bin/sh
# Print Aura's version, from the line the About box reads
# (AU_VERSION in src/browser/credits.h).
set -eu
sed -n 's/^#define AU_VERSION "\([^"]*\)".*$/\1/p' \
    "$(dirname "$0")/../src/browser/credits.h"
