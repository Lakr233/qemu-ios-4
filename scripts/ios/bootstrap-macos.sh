#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if [ -n "${PYTHON:-}" ]; then
    iphone3g_python=$PYTHON
else
    iphone3g_python=
    for candidate in python3.13 python3.12 python3.11 python3; do
        if command -v "$candidate" >/dev/null 2>&1; then
            iphone3g_python=$candidate
            break
        fi
    done
fi

if [ -z "$iphone3g_python" ]; then
    echo "Python 3.11 or newer is required" >&2
    exit 1
fi

if ! "$iphone3g_python" -c '
import sys
if sys.version_info < (3, 11):
    raise SystemExit(1)
'; then
    echo "$iphone3g_python must be Python 3.11 or newer" >&2
    exit 1
fi

cd "$root"
echo "Using $iphone3g_python ($("$iphone3g_python" --version))"
"$iphone3g_python" -m venv .venv
.venv/bin/python -m pip install -r scripts/ios/requirements.txt

./configure \
    --python="$root/.venv/bin/python" \
    --target-list=arm-softmmu \
    --enable-tcg \
    --disable-hvf \
    --enable-cocoa \
    --disable-werror \
    --prefix="$root/build/install"
ninja -C build
