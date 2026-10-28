#!/bin/sh
# Exercises the display host driver without a framebuffer or Wayland server.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out=$(mktemp -d /tmp/pbl-display-review.XXXXXX)
trap 'rm -rf "$out"' EXIT
cc=${CC:-cc}
set -- -std=c2x -Wall -Wextra -Werror -O1 -g "-I$root/include" \
  "-I$root/soc/posix/include"
if [ "$(uname -s)" = Darwin ]; then
  headers=${LINUX_HEADERS:-$root/../tools/python/ziglang/lib/libc/include/any-linux-any}
  if [ ! -f "$headers/linux/fb.h" ]; then
    echo 'Set LINUX_HEADERS to a Linux UAPI include directory for the macOS host test.' >&2
    exit 2
  fi
  set -- "$@" -idirafter "$headers"
fi
"$cc" "$@" "$here/test_display_deadline.c" -o "$out/test_display_deadline" -lpthread -lm
"$out/test_display_deadline"
