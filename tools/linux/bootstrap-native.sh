#!/bin/sh
# Fetch only the source dependencies needed by native builds and their unit tests.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
git submodule update --init --depth 1 -- \
  third_party/cmsis_core/CMSIS third_party/nanopb/nanopb third_party/tinymt/TinyMT \
  third_party/qr_code_generator/QR-Code-generator third_party/speex/speex \
  third_party/mbedtls/mbedtls third_party/uzlib/uzlib third_party/upng/upng \
  third_party/nimble/mynewt-nimble third_party/samsa/samsa \
  third_party/resources/iconography third_party/fff/fff
patch=$root/patches/nimble-linux.patch
if [ -s "$patch" ]; then
  module=$root/third_party/nimble/mynewt-nimble
  if git -C "$module" apply --reverse --check "$patch" 2>/dev/null; then
    echo 'NimBLE Linux transport patch is already applied.'
  else
    git -C "$module" apply --check "$patch"
    git -C "$module" apply "$patch"
  fi
fi
