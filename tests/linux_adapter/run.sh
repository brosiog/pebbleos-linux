#!/bin/sh
# Builds and runs the adapter protocol tests on the host. A C11 compiler with pthreads is enough;
# no watch and no firmware toolchain.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${BUILD_DIR:-$(mktemp -d)}
cc=${CC:-cc}

build() {
  target=$1
  shift
  "$cc" -std=gnu11 -Wall -Wextra -Werror -O1 -g -DLINUX_ADAPTER_STANDALONE \
    -I"$root/include" -I"$here" -o "$out/$target" "$@" -lpthread
}

build test_client "$here/test_client.c" "$here/mock_adapter.c" \
  "$root/drivers/linux_adapter/linux_adapter_client.c" \
  "$root/drivers/linux_adapter/linux_profile.c"
build mock_adapter "$here/mock_adapter_main.c" "$here/mock_adapter.c"
build conformance "$here/conformance.c"

"$out/test_client"
sh "$here/run_regressions.sh"
sh "$here/self_conformance.sh" "$out"
