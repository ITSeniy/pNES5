#!/bin/sh
set -u

frames=${FRAMES:-1200}
runner=${RUNNER:-./tests/nes_romtest}

if [ "$#" -lt 1 ]; then
    echo "usage: $0 'glob/of/tests/*.nes' [more/globs/*.nes]" >&2
    exit 2
fi

status=0
for pattern in "$@"; do
    for rom in $pattern; do
        [ -f "$rom" ] || continue
        printf '%s: ' "$rom"
        out=$("$runner" "$rom" --frames "$frames" --expect-pass 2>&1)
        rc=$?
        if [ "$rc" -eq 0 ]; then
            echo PASS
        else
            echo FAIL
            echo "$out" | tail -n 4
            status=1
        fi
    done
done

exit "$status"
