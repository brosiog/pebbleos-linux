#!/bin/sh
# Runs the conformance checker against the mock adapter, to prove the checker itself.
set -eu
out=$1
dir=$(mktemp -d /tmp/pbl-conf.XXXXXX)
trap 'kill "$pid" 2>/dev/null || true; rm -rf "$dir"' EXIT
"$out/mock_adapter" --dir "$dir" --caps 0x3ff --latency 20 --fast-path 2>/dev/null &
pid=$!
sleep 0.3
"$out/conformance" --dir "$dir" --bound-ms 1500
