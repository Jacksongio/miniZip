#!/bin/sh
# miniZip conformance tests.
#
# Every archive produced here is validated with `unzip -t` (CRC check) and,
# where possible, round-tripped and diffed against the original tree. The
# point is not that miniZip agrees with itself but that a third-party
# extractor agrees with miniZip.
#
# Usage: MINIZIP=./build/minizip sh tests/run_tests.sh

set -u

MINIZIP=${MINIZIP:-./build/minizip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/minizip-test.XXXXXX")
PASS=0
FAIL=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

ok()   { PASS=$((PASS+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
check(){ if [ "$2" = 0 ]; then ok "$1"; else bad "$1"; fi }

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "skip: '$1' not installed"; return 1; }
}

if [ ! -x "$MINIZIP" ]; then
    echo "error: '$MINIZIP' not found or not executable (run 'make' first)" >&2
    exit 1
fi
need unzip || exit 0

echo "miniZip test suite  ($MINIZIP)"
echo

# ---------------------------------------------------------------- fixtures
mkdir -p "$WORK/tree/sub/deeper" "$WORK/tree/empty-dir"
printf 'hello world\n'                      > "$WORK/tree/small.txt"
: > "$WORK/tree/zero.txt"
# ~3 MiB of highly compressible text
i=0; while [ $i -lt 40000 ]; do
    printf 'the quick brown fox jumps over the lazy dog %s\n' "$i"
    i=$((i+1))
done > "$WORK/tree/sub/big.txt"
# 2 MiB of incompressible noise
dd if=/dev/urandom of="$WORK/tree/sub/deeper/noise.bin" bs=1024 count=2048 2>/dev/null
# a name that needs the UTF-8 flag
printf 'unicode payload\n'                  > "$WORK/tree/naïve-café.txt"
ln -sf small.txt "$WORK/tree/link.txt" 2>/dev/null || true

# --------------------------------------------------------------- test 1
"$MINIZIP" -q "$WORK/t1.zip" "$WORK/tree/small.txt" 2>/dev/null
unzip -tqq "$WORK/t1.zip" >/dev/null 2>&1
check "single file archive passes unzip -t" $?

# --------------------------------------------------------------- test 2
"$MINIZIP" -q -r "$WORK/t2.zip" "$WORK/tree" 2>/dev/null
unzip -tqq "$WORK/t2.zip" >/dev/null 2>&1
check "recursive archive passes unzip -t" $?

# --------------------------------------------------------------- test 3
mkdir -p "$WORK/out3"
( cd "$WORK/out3" && unzip -qq "$WORK/t2.zip" ) 2>/dev/null
STRIPPED=$(printf '%s' "${WORK#/}")
if diff -r "$WORK/tree" "$WORK/out3/$STRIPPED/tree" >/dev/null 2>&1; then
    ok "round-trip is byte-identical to the source tree"
else
    bad "round-trip is byte-identical to the source tree"
    diff -rq "$WORK/tree" "$WORK/out3/$STRIPPED/tree" 2>&1 | head -5
fi

# --------------------------------------------------------------- test 4
# Block boundaries depend only on -b, never on scheduling, so every thread
# count must emit a byte-for-byte identical archive.
FAILED=0
( cd "$WORK" && "$MINIZIP" -q -T 0 -b 64K "$WORK/t4-ref.zip" tree/sub/big.txt ) 2>/dev/null
REF=$(cksum < "$WORK/t4-ref.zip")
for T in 1 2 4 8 16; do
    ( cd "$WORK" && "$MINIZIP" -q -T "$T" -b 64K "$WORK/t4-$T.zip" tree/sub/big.txt ) 2>/dev/null
    [ "$(cksum < "$WORK/t4-$T.zip")" = "$REF" ] || FAILED=1
    unzip -tqq "$WORK/t4-$T.zip" >/dev/null 2>&1 || FAILED=1
done
check "output is byte-identical at T=0,1,2,4,8,16" $FAILED

# Contents must also match the source, not merely each other.
[ "$(unzip -p "$WORK/t4-8.zip" | cksum)" = "$(cksum < "$WORK/tree/sub/big.txt")" ]
check "parallel-deflated data inflates back to the original" $?

# --------------------------------------------------------------- test 5
# Multi-block dictionary priming: tiny blocks force many Z_SYNC_FLUSH joins.
( cd "$WORK" && "$MINIZIP" -q -9 -T 4 -b 64K "$WORK/t5.zip" tree/sub/big.txt ) 2>/dev/null
GOT=$(unzip -p "$WORK/t5.zip" | cksum)
EXP=$(cksum < "$WORK/tree/sub/big.txt")
[ "$GOT" = "$EXP" ]
check "64 KiB blocks at level 9 reconstruct the original exactly" $?

# --------------------------------------------------------------- test 6
"$MINIZIP" -q "$WORK/t6.zip" "$WORK/tree/zero.txt" 2>/dev/null
unzip -tqq "$WORK/t6.zip" >/dev/null 2>&1 && [ "$(unzip -p "$WORK/t6.zip" | wc -c)" = "0" ]
check "empty file produces a valid, empty member" $?

# --------------------------------------------------------------- test 7
"$MINIZIP" -q -j "$WORK/t7.zip" "$WORK/tree/sub/deeper/noise.bin" 2>/dev/null
METHOD=$(unzip -v "$WORK/t7.zip" | awk '/noise.bin/ {print $2}')
[ "$METHOD" = "Stored" ]
check "incompressible data is detected and stored, not deflated" $?

# --------------------------------------------------------------- test 8
NAMES=$(unzip -Z1 "$WORK/t7.zip")
[ "$NAMES" = "noise.bin" ]
check "-j strips directory components" $?

# --------------------------------------------------------------- test 9
"$MINIZIP" -q -s "$WORK/t9.zip" "$WORK/tree/sub/big.txt" 2>/dev/null
unzip -tqq "$WORK/t9.zip" >/dev/null 2>&1 &&
  [ "$(unzip -v "$WORK/t9.zip" | awk '/big.txt/ {print $2}')" = "Stored" ]
check "-s stores everything" $?

# --------------------------------------------------------------- test 10
"$MINIZIP" -q -j "$WORK/t10.zip" "$WORK/tree/naïve-café.txt" 2>/dev/null
unzip -tqq "$WORK/t10.zip" >/dev/null 2>&1
check "non-ASCII names round-trip with the UTF-8 flag" $?

# --------------------------------------------------------------- test 11
# Archive names must never escape the extraction directory.
( cd "$WORK/tree" && "$MINIZIP" -q "$WORK/t11.zip" ../tree/small.txt ) 2>/dev/null
BADNAME=$(unzip -Z1 "$WORK/t11.zip" | grep -c -e '^/' -e '\.\.' || true)
[ "$BADNAME" = "0" ]
check "'..' and leading '/' are stripped from stored names" $?

# --------------------------------------------------------------- test 12
# -r must not try to swallow the archive it is writing.
mkdir -p "$WORK/self"
cp "$WORK/tree/small.txt" "$WORK/self/"
( cd "$WORK/self" && "$MINIZIP" -q -r self.zip . ) 2>/dev/null
unzip -tqq "$WORK/self/self.zip" >/dev/null 2>&1 &&
  [ "$(unzip -Z1 "$WORK/self/self.zip" | grep -c 'self.zip' || true)" = "0" ]
check "-r skips the archive being written" $?

# --------------------------------------------------------------- test 13
for L in 0 1 6 9; do
    "$MINIZIP" -q "-$L" "$WORK/t13.zip" "$WORK/tree/sub/big.txt" 2>/dev/null
    unzip -tqq "$WORK/t13.zip" >/dev/null 2>&1 || FAILED=1
done
check "levels 0/1/6/9 all produce valid archives" ${FAILED:-0}

# --------------------------------------------------------------- test 14
if need zipinfo; then
    "$MINIZIP" -q -r "$WORK/t14.zip" "$WORK/tree" 2>/dev/null
    zipinfo "$WORK/t14.zip" >/dev/null 2>&1
    check "zipinfo parses the central directory" $?
fi

# --------------------------------------------------------------- test 15
# Unix permission bits survive the round trip.
chmod 700 "$WORK/tree/small.txt"
"$MINIZIP" -q -j "$WORK/t15.zip" "$WORK/tree/small.txt" 2>/dev/null
PERM=$(unzip -Z "$WORK/t15.zip" | awk '/small.txt/ {print substr($1,2,9)}')
[ "$PERM" = "rwx------" ]
check "Unix permissions are stored in the external attributes" $?
chmod 644 "$WORK/tree/small.txt"

# --------------------------------------------------------------- test 16
# A second, independent implementation must agree about the container.
if command -v python3 >/dev/null 2>&1; then
    "$MINIZIP" -q -r "$WORK/t16.zip" "$WORK/tree" 2>/dev/null
    python3 - "$WORK/t16.zip" <<'PY' >/dev/null 2>&1
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None, "CRC mismatch"
names = z.namelist()
assert any(n.endswith('/') for n in names),           "no directory entry"
assert any('caf' in n for n in names),                "utf-8 name missing"
# the UTF-8 general-purpose bit must be set exactly on non-ASCII names
for i in z.infolist():
    nonascii = any(ord(c) > 127 for c in i.filename)
    assert bool(i.flag_bits & 0x800) == nonascii, f"utf8 flag wrong on {i.filename}"
    assert i.create_system == 3,                  f"made-by is not UNIX on {i.filename}"
PY
    check "python3 zipfile agrees about CRCs, flags and attributes" $?
else
    echo "  skip: python3 not installed"
fi

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
