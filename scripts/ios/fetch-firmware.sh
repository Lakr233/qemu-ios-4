#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
name='iPhone1,2_4.2.1_8C148_Restore.ipsw'
url='https://secure-appldnld.apple.com/iPhone4/061-9853.20101122.Vfgt5/iPhone1,2_4.2.1_8C148_Restore.ipsw'
expected_size='338579762'
expected_sha256='98e5969c3baed660c9a26e94cd7ed4b3cdb7175900f448bcc2223bf885835ce0'
directory=${1:-"$root/.artifacts/firmware"}
destination="$directory/$name"
partial="$destination.partial"

mkdir -p "$directory"

validate_firmware()
{
    firmware=$1
    actual_size=$(wc -c < "$firmware" | tr -d ' ')
    if [ "$actual_size" != "$expected_size" ]; then
        echo "unexpected firmware size: $actual_size" \
            "(expected $expected_size)" >&2
        return 1
    fi

    actual_sha256=$(shasum -a 256 "$firmware" | awk '{print $1}')
    if [ "$actual_sha256" != "$expected_sha256" ]; then
        echo "unexpected firmware SHA-256: $actual_sha256" >&2
        return 1
    fi
}

if [ ! -f "$destination" ]; then
    echo "Downloading the unmodified IPSW from Apple's HTTPS CDN." >&2
    curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --continue-at - --retry 3 --output "$partial" "$url"
    if ! validate_firmware "$partial"; then
        rm -f "$partial"
        exit 1
    fi
    mv "$partial" "$destination"
fi

validate_firmware "$destination"

echo "$destination"
