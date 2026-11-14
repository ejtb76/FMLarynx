#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# FMLarynx's tests, as the release workflow runs them (and anyone, in WSL or on Linux): the host simulator runs
# every script in app/tests/; a script fails on a FAIL line or a non-zero exit.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build/tests"
mkdir -p "$OUT"
sh build.sh --host >"$OUT/host-build.log" 2>&1 || { cat "$OUT/host-build.log"; exit 1; }
fails=0
for t in app/tests/*.txt; do
    name="$(basename "$t" .txt)"
    mkdir -p "$OUT/$name"
    if app/build/host/fmlarynx_host "$t" "$OUT/$name" >"$OUT/$name.log" 2>&1 && ! grep -q '^ *FAIL' "$OUT/$name.log"; then
        echo "ok    $name"
    else
        echo "FAIL  $name"
        grep '^ *FAIL' "$OUT/$name.log" || tail -5 "$OUT/$name.log"
        fails=$((fails + 1))
    fi
done
[ "$fails" = 0 ] || { echo "$fails failed"; exit 1; }
echo "all passed"
