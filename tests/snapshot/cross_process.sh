#!/bin/sh
# Run each script normally, then in slices where every slice is a new
# process restoring the runtime from the base and the state (a delta of
# the base) saved by the previous one (see hibernate.c). Both outputs must
# be identical.
#
#   cross_process.sh path/to/hibernate script.js...
set -e
bin=$1
shift
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0
for script in "$@"; do
    "$bin" plain "$script" > "$tmp/expected"
    slices=1
    max_state=0
    if "$bin" start "$script" "$tmp/base" "$tmp/state" > "$tmp/got"; then
        rc=0
    else
        rc=$?
    fi
    while [ $rc -eq 2 ]; do
        size=$(wc -c < "$tmp/state" | tr -d ' ')
        [ "$size" -gt "$max_state" ] && max_state=$size
        slices=$((slices + 1))
        if "$bin" resume "$tmp/base" "$tmp/state" >> "$tmp/got"; then
            rc=0
        else
            rc=$?
        fi
    done
    if [ $rc -eq 0 ] && cmp -s "$tmp/expected" "$tmp/got"; then
        echo "ok   $script ($slices processes, base $(wc -c < "$tmp/base" | tr -d ' ') bytes, max state $max_state bytes)"
    else
        echo "FAIL $script (status $rc)"
        diff "$tmp/expected" "$tmp/got" | head -20 || true
        status=1
    fi
done
exit $status
