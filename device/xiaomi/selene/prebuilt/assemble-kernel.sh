#!/bin/sh
# Join the kernel parts into prebuilt/Image.gz.
#
# The prebuilt is dieD's 4.19.325 Image.gz, 12384187 bytes. That is over the
# 8 MB ceiling the GitHub contents/blob API enforces for a single file, so the
# tree stores it as Image.gz.00 + Image.gz.01 and this script joins them. CI
# runs it before Build; a local build needs to run it once too.
#
#   ./prebuilt/assemble-kernel.sh
#
# The size and md5 checks are the point: a truncated part produces a kernel
# that links and then does not boot, and that is a 350 minute round trip to
# discover.
set -e

cd "$(dirname "$0")"

EXPECTED_SIZE=12384187
EXPECTED_MD5=83b5653e0017301f84b441e600e8851d

cat Image.gz.00 Image.gz.01 > Image.gz

size=$(wc -c < Image.gz | tr -d ' ')
if [ "${size}" != "${EXPECTED_SIZE}" ]; then
    echo "Image.gz is ${size} bytes, expected ${EXPECTED_SIZE}" >&2
    exit 1
fi

if command -v md5sum >/dev/null 2>&1; then
    md5=$(md5sum Image.gz | cut -d' ' -f1)
elif command -v md5 >/dev/null 2>&1; then
    md5=$(md5 -q Image.gz)
else
    md5=""
fi

if [ -n "${md5}" ] && [ "${md5}" != "${EXPECTED_MD5}" ]; then
    echo "Image.gz md5 is ${md5}, expected ${EXPECTED_MD5}" >&2
    exit 1
fi

if [ -n "${md5}" ]; then
    echo "Image.gz assembled: ${size} bytes, md5 ${md5}"
else
    echo "Image.gz assembled: ${size} bytes"
fi
