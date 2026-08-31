#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
upstream="$root/.artifacts/tools/usbmuxd"
output=${1:-"$root/build/usbmuxd-usboip"}
revision=3ded00c9985a5108cfc7591a309f9a23d57a8cba
legacy_serial_patch="$root/scripts/ios/usbmuxd-legacy-serial.patch"

if test ! -d "$upstream/.git"; then
    mkdir -p "$(dirname "$upstream")"
    git clone https://github.com/libimobiledevice/usbmuxd.git "$upstream"
    git -C "$upstream" checkout --detach "$revision"
fi

actual=$(git -C "$upstream" rev-parse HEAD)
if test "$actual" != "$revision"; then
    echo "usbmuxd source is $actual, expected $revision" >&2
    echo "remove $upstream or check out the pinned revision" >&2
    exit 1
fi

if git -C "$upstream" apply --check "$legacy_serial_patch" 2>/dev/null; then
    git -C "$upstream" apply "$legacy_serial_patch"
elif ! git -C "$upstream" apply --reverse --check "$legacy_serial_patch" 2>/dev/null; then
    echo "usbmuxd source does not match the legacy serial patch" >&2
    exit 1
fi

if test ! -x "$upstream/configure"; then
    (cd "$upstream" && NOCONFIGURE=1 ./autogen.sh)
fi
if test ! -f "$upstream/config.h"; then
    (cd "$upstream" && ./configure --without-systemd --without-udev)
fi

packages="libplist-2.0 libimobiledevice-glue-1.0 libusb-1.0 libimobiledevice-1.0"
cflags=$(pkg-config --cflags $packages)
libs=$(pkg-config --libs $packages)
mkdir -p "$(dirname "$output")"

${CC:-cc} -std=gnu11 -O2 -g -DHAVE_CONFIG_H \
    -I"$upstream" -I"$upstream/src" -I"$upstream/include" \
    $cflags \
    "$upstream/src/client.c" \
    "$upstream/src/device.c" \
    "$upstream/src/preflight.c" \
    "$upstream/src/log.c" \
    "$upstream/src/utils.c" \
    "$upstream/src/conf.c" \
    "$upstream/src/main.c" \
    "$root/scripts/ios/usbmuxd-usboip.c" \
    $libs -lpthread -o "$output.tmp"
mv "$output.tmp" "$output"
echo "built $output from usbmuxd $revision"
