#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
out=$(mktemp -d /tmp/pbl-review.XXXXXX)
trap 'rm -rf "$out"' EXIT
cc=${CC:-cc}
set -- -std=gnu11 -Wall -Wextra -Werror -O1 -g -DLINUX_ADAPTER_STANDALONE \
  "-I$root/include" "-I$here"
"$cc" "$@" -o "$out/test_bind_race" "$here/test_bind_race.c"
"$cc" "$@" -o "$out/test_conformance_checks" "$here/test_conformance_checks.c"
"$cc" "$@" -o "$out/test_client_regressions" "$here/test_client_regressions.c" \
  "$here/mock_adapter.c" "$root/drivers/linux_adapter/linux_adapter_client.c" \
  "$root/drivers/linux_adapter/linux_profile.c" -lpthread
failed=0
"$out/test_bind_race" || failed=1
"$out/test_client_regressions" || failed=1
"$out/test_conformance_checks" || failed=1
exit "$failed"
