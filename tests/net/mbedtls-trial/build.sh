#!/bin/sh
# Build the Mbed TLS trial library with the trial configuration.
#
# Usage: tests/net/mbedtls-trial/build.sh MBEDTLS_DIR OUT_DIR [host|ppc] [extra compiler flags]
#
# MBEDTLS_DIR is an unpacked Mbed TLS 4.1.x release (it carries
# TF-PSA-Crypto in tf-psa-crypto/). The script compiles the library
# sources itself; it runs none of the release's own build scripts.
# "ppc" compiles with the Retro68 PowerPC compiler (toolchain/find.sh).
# Add -DAURA_TRIAL_EVEREST for the Everest X25519 code.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
M=$(cd "$1" && pwd)
out=$2
target=${3:-host}
shift; shift; [ $# -gt 0 ] && shift
mkdir -p "$out"
out=$(cd "$out" && pwd)
T=$M/tf-psa-crypto
if [ "$target" = ppc ]; then
    . "$here/../../../toolchain/find.sh"
    CC=$RETRO68/bin/powerpc-apple-macos-gcc
    AR=$RETRO68/bin/powerpc-apple-macos-ar
else
    CC=${CC:-cc}
    AR=${AR:-ar}
fi
inc="-I$here -I$M/include -I$M/library -I$T/include -I$T/core -I$T/dispatch -I$T/extras -I$T/platform -I$T/utilities -I$T/drivers/builtin/include -I$T/drivers/builtin/src -I$T/drivers/everest/include -I$T/drivers/everest/include/tf-psa-crypto/private/everest -I$T/drivers/everest/include/tf-psa-crypto/private/everest/kremlib"
def="-DMBEDTLS_CONFIG_FILE=\"aura_mbedtls_config.h\" -DTF_PSA_CRYPTO_CONFIG_FILE=\"aura_crypto_config.h\""
rm -f "$out"/*.o "$out/libmbedtrial.a"
fail=0
for f in "$M"/library/*.c "$T"/core/*.c "$T"/extras/*.c "$T"/platform/*.c "$T"/utilities/*.c "$T"/drivers/builtin/src/*.c "$T"/drivers/everest/library/x25519.c "$T"/drivers/everest/library/Hacl_Curve25519_joined.c; do
    o=$out/$(basename "$f" .c).o
    if ! $CC -std=c99 -Os -ffunction-sections -fdata-sections $inc $def "$@" -c "$f" -o "$o" 2> "$o.err"; then
        echo "FAILED $f"
        head -12 "$o.err"
        fail=1
    elif [ -s "$o.err" ]; then
        echo "warnings in $f: $(grep -c 'warning:' "$o.err")"
    else
        rm -f "$o.err"
    fi
done
[ $fail = 0 ] || exit 1
$AR rcs "$out/libmbedtrial.a" "$out"/*.o
echo "built $out/libmbedtrial.a"
