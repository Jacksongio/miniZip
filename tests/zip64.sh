#!/bin/sh
# ZIP64 tests. Separate from run_tests.sh because they need ~11 GB of scratch
# space and a minute or two of wall clock.
#
# Usage: MINIZIP=./build/minizip sh tests/zip64.sh

set -u

MINIZIP=${MINIZIP:-./build/minizip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/minizip-zip64.XXXXXX")
PASS=0
FAIL=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

ok()   { PASS=$((PASS+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
check(){ if [ "$2" = 0 ]; then ok "$1"; else bad "$1"; fi }

[ -x "$MINIZIP" ] || { echo "error: '$MINIZIP' not found (run 'make')" >&2; exit 1; }
command -v unzip >/dev/null 2>&1 || { echo "skip: unzip not installed"; exit 0; }

echo "miniZip ZIP64 tests  ($MINIZIP)"
echo

# --- a single member larger than 4 GiB -------------------------------------
# 5 GiB of zeros deflates to a couple of megabytes, so only the *uncompressed*
# size crosses the 32-bit boundary. That is the ZIP64 local-header path.
echo "  ... building a 5 GiB member"
dd if=/dev/zero of="$WORK/huge.bin" bs=1M count=5120 2>/dev/null
"$MINIZIP" -q -1 -j "$WORK/z1.zip" "$WORK/huge.bin"
unzip -tqq "$WORK/z1.zip" >/dev/null 2>&1
check "member > 4 GiB passes unzip -t" $?

SIZE=$(unzip -v "$WORK/z1.zip" | awk '/huge.bin/ {print $1}')
[ "$SIZE" = "5368709120" ]
check "uncompressed size is reported as 5368709120, not truncated" $?

rm -f "$WORK/huge.bin"

# --- an archive larger than 4 GiB ------------------------------------------
# Stored random data pushes the second member's local header past 4 GiB, which
# exercises the ZIP64 offset field plus the EOCD64 record and its locator.
echo "  ... building a > 4 GiB archive"
dd if=/dev/urandom of="$WORK/rand.bin" bs=1M count=4200 2>/dev/null
printf 'tail member\n' > "$WORK/tail.txt"
"$MINIZIP" -q -s -j "$WORK/z2.zip" "$WORK/rand.bin" "$WORK/tail.txt"
unzip -tqq "$WORK/z2.zip" >/dev/null 2>&1
check "archive > 4 GiB passes unzip -t" $?

unzip -p "$WORK/z2.zip" tail.txt 2>/dev/null | grep -q 'tail member'
check "member at an offset > 4 GiB extracts correctly" $?

rm -f "$WORK/rand.bin" "$WORK/z2.zip"

# --- more than 65535 entries -----------------------------------------------
echo "  ... building 70000 entries"
mkdir -p "$WORK/many"
i=0
while [ $i -lt 70000 ]; do
    printf 'entry %s\n' "$i" > "$WORK/many/f$i"
    i=$((i+1))
done
"$MINIZIP" -q -r "$WORK/z3.zip" "$WORK/many"
unzip -tqq "$WORK/z3.zip" >/dev/null 2>&1
check "70000 entries pass unzip -t" $?

COUNT=$(unzip -Z1 "$WORK/z3.zip" | wc -l)
[ "$COUNT" -eq 70001 ]     # 70000 files + the directory entry
check "all 70000 entries are listed via the EOCD64 record" $?

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
