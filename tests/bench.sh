#!/bin/sh
# Rough throughput/ratio comparison against Info-ZIP's zip(1).
#
# Usage: MINIZIP=./build/minizip sh tests/bench.sh [corpus-dir]
#
# With no corpus directory a synthetic one is generated: a large text file,
# a large binary blob, and a pile of small source-like files.

set -u

MINIZIP=${MINIZIP:-./build/minizip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/minizip-bench.XXXXXX")
CORPUS=${1:-}

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

[ -x "$MINIZIP" ] || { echo "error: '$MINIZIP' not found (run 'make')" >&2; exit 1; }

if [ -z "$CORPUS" ]; then
    CORPUS="$WORK/corpus"
    mkdir -p "$CORPUS/small"
    echo "generating a synthetic corpus ..."
    i=0
    while [ $i -lt 600000 ]; do
        printf '%s the quick brown fox jumps over the lazy dog 0123456789\n' "$i"
        i=$((i+1))
    done > "$CORPUS/text.log"
    dd if=/dev/urandom of="$CORPUS/blob.bin" bs=1M count=64 2>/dev/null
    i=0
    while [ $i -lt 400 ]; do
        sed -n '1,400p' "$CORPUS/text.log" > "$CORPUS/small/part$i.txt"
        i=$((i+1))
    done
fi

BYTES=$(du -sb "$CORPUS" 2>/dev/null | cut -f1)
echo "corpus: $CORPUS  ($BYTES bytes)"
echo

run() {
    label=$1; shift
    out=$1; shift
    rm -f "$out"
    start=$(date +%s.%N)
    "$@" >/dev/null 2>&1
    end=$(date +%s.%N)
    secs=$(awk "BEGIN{printf \"%.2f\", $end-$start}")
    size=$(wc -c < "$out")
    awk -v l="$label" -v s="$secs" -v z="$size" -v b="$BYTES" \
        'BEGIN{printf "  %-26s %7.2fs  %12d bytes  %5.1f%% saved  %6.1f MiB/s\n",
               l, s, z, 100*(1-z/b), (b/1048576)/(s>0?s:0.001)}'
}

CPUS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

run "minizip -6 -T $CPUS" "$WORK/mz6.zip"  "$MINIZIP" -q -6 -r "$WORK/mz6.zip" "$CORPUS"
run "minizip -6 -T 0"     "$WORK/mz6s.zip" "$MINIZIP" -q -6 -T 0 -r "$WORK/mz6s.zip" "$CORPUS"
run "minizip -9 -T $CPUS" "$WORK/mz9.zip"  "$MINIZIP" -q -9 -r "$WORK/mz9.zip" "$CORPUS"
run "minizip -1 -T $CPUS" "$WORK/mz1.zip"  "$MINIZIP" -q -1 -r "$WORK/mz1.zip" "$CORPUS"

if command -v zip >/dev/null 2>&1; then
    run "zip -6 (Info-ZIP)" "$WORK/iz6.zip" zip -q -6 -r "$WORK/iz6.zip" "$CORPUS"
    run "zip -9 (Info-ZIP)" "$WORK/iz9.zip" zip -q -9 -r "$WORK/iz9.zip" "$CORPUS"
else
    echo "  (zip(1) not installed, skipping the comparison)"
fi
