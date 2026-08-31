#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:-"$root/build/iphone3g-ppp-nat"}

if ! pkg-config --exists slirp; then
    echo "libslirp development files are required (macOS: brew install libslirp)" >&2
    exit 1
fi

cflags=$(pkg-config --cflags slirp)
libs=$(pkg-config --libs slirp)
mkdir -p "$(dirname "$output")"

${CC:-cc} -std=c11 -O2 -g -Wall -Wextra -Werror \
    $cflags \
    "$root/scripts/ios/iphone3g-ppp.c" \
    "$root/scripts/ios/iphone3g-ppp-nat.c" \
    $libs -o "$output.tmp"
mv "$output.tmp" "$output"
echo "built $output with $(pkg-config --modversion slirp)"
