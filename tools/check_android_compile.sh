#!/bin/sh
# Syntax/type check of the Android backend on the build host.
#
# This is NOT a substitute for the NDK build: it compiles the new sources with
# a glibc cross compiler against stub Android/JNI headers, which catches typos,
# type errors and missing includes without downloading the NDK.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-arm-linux-gnueabi-gcc}
OUT=${OUT:-/tmp/primeshim-android-check}

mkdir -p "$OUT"

CFLAGS="-std=gnu11 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -O1 -Wall -Wextra
        -Wno-unused-parameter -I$ROOT/android/check-stubs -I$ROOT/include -I$ROOT/android/jni
        -DPRIMESHIM_HOST_PC=0 -DPRIMESHIM_HOST_DEVICE=0 -DPRIMESHIM_HOST_ANDROID=1
        -DPRIMESHIM_DATA_DIR=\"prime_data\""

# shellcheck disable=SC2086
$CC $CFLAGS -c "$ROOT/src/platform/lcd_android.c" -o "$OUT/lcd_android.o"
# shellcheck disable=SC2086
for source in "$ROOT"/android/jni/*.c; do
    # shellcheck disable=SC2086
    $CC $CFLAGS -c "$source" -o "$OUT/$(basename "$source" .c).o"
done

echo "android backend compiles (stubbed headers): $OUT"
