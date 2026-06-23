#!/usr/bin/env bash
#
# taput regression suite
#
# Self-contained: generates its own fixtures in a temp dir and exercises the
# core invariants that must always hold. No external corpus required.
#
# Usage: bash scripts/regression.sh [path-to-taput]   (default: ./taput)

set -u

TAPUT="${1:-./taput}"
PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$TAPUT" ]; then
    echo "E : taput binary not found/executable at '$TAPUT'"
    echo "    Build it first (make) or pass the path as argument."
    exit 2
fi
# Resolve to an absolute path: the tests run from inside a temp dir.
case "$TAPUT" in
    /*) ;;
    *)  TAPUT="$(cd "$(dirname "$TAPUT")" && pwd)/$(basename "$TAPUT")" ;;
esac

# ok <description> <condition-exit-code>
ok() {
    if [ "$2" -eq 0 ]; then
        PASS=$((PASS + 1))
        printf 'PASS  %s\n' "$1"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL  %s\n' "$1"
    fi
}

# Generate deterministic fixture data (no randomness -> reproducible).
gen() { # gen <file> <size>
    perl -e 'print chr($_ % 256) for 0..($ARGV[0]-1)' "$2" > "$1"
}

cd "$TMP" || exit 2

gen code.bin 1000
gen screen.scr 6912

#---------------------------------------------------------------------------
# 1. add (with CODE header) then extract -> payload byte-identical
#---------------------------------------------------------------------------
"$TAPUT" add -o 32768 -n "MyCode" code.bin img.tap >/dev/null 2>&1
ok "add creates a tap image" $?

"$TAPUT" extract -s 2 img.tap out.bin >/dev/null 2>&1
cmp -s code.bin out.bin
ok "add -> extract round-trips payload byte-identically" $?

#---------------------------------------------------------------------------
# 2. list / info see the header
#---------------------------------------------------------------------------
"$TAPUT" list img.tap 2>/dev/null | grep -q "CODE 32768,1000"
ok "list reports the CODE header (addr,len)" $?

"$TAPUT" info img.tap 2>/dev/null | grep -q "Integrity     : OK"
ok "info reports integrity OK on a good image" $?

"$TAPUT" info img.tap 2>/dev/null | grep -q "Data bytes    : 1000"
ok "info counts data bytes correctly" $?

#---------------------------------------------------------------------------
# 3. export-all + .inf, then rebuild via --from-inf -> byte-identical tap
#---------------------------------------------------------------------------
mkdir -p ex
"$TAPUT" export-all --inf img.tap ./ex >/dev/null 2>&1
ok "export-all writes files" $?

ls ex/*.inf >/dev/null 2>&1
ok "export-all --inf writes a sidecar" $?

INF="$(ls ex/*.inf 2>/dev/null | head -1)"
DAT="${INF%.inf}"
rm -f rebuilt.tap
"$TAPUT" add --from-inf "$INF" "$DAT" rebuilt.tap >/dev/null 2>&1
cmp -s img.tap rebuilt.tap
ok ".inf round-trip rebuilds a byte-identical tap" $?

#---------------------------------------------------------------------------
# 4. SCREEN$ detection + export-png produces a valid PNG
#---------------------------------------------------------------------------
"$TAPUT" add -o 16384 -n "scr" screen.scr scr.tap >/dev/null 2>&1
"$TAPUT" export-all scr.tap ./scrout >/dev/null 2>&1
ls scrout/*.scr >/dev/null 2>&1
ok "export-all auto-detects a SCREEN\$ (.scr extension)" $?

"$TAPUT" export-png -s 2 scr.tap shot.png >/dev/null 2>&1
ok "export-png runs on a SCREEN\$ block" $?

# PNG signature check (89 50 4E 47); strip whitespace for portable od output.
SIG="$(head -c 4 shot.png 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' \n')"
[ "$SIG" = "89504e47" ]
ok "export-png output has a valid PNG signature" $?

"$TAPUT" export-png screen.scr shot2.png >/dev/null 2>&1
cmp -s shot.png shot2.png
ok "export-png from .tap block and from .scr file are identical" $?

#---------------------------------------------------------------------------
# 5. fix-crc repairs a tampered checksum
#---------------------------------------------------------------------------
cp img.tap tampered.tap
# Corrupt the final byte (the data block's checksum) using perl.
perl -e 'open$f,"+<",$ARGV[0];seek$f,-1,2;print$f chr(0xAA);close$f' tampered.tap
"$TAPUT" info tampered.tap 2>/dev/null | grep -q "Bad checksums"
ok "info detects a bad checksum" $?

"$TAPUT" fix-crc -s 2 tampered.tap >/dev/null 2>&1
"$TAPUT" info tampered.tap 2>/dev/null | grep -q "Integrity     : OK"
ok "fix-crc repairs the checksum" $?

cmp -s img.tap tampered.tap
ok "fix-crc restores the original bytes" $?

#---------------------------------------------------------------------------
# 6. remove drops a block
#---------------------------------------------------------------------------
cp img.tap rem.tap
"$TAPUT" remove -s 1 rem.tap >/dev/null 2>&1
BEFORE=$("$TAPUT" info img.tap 2>/dev/null | grep Blocks | grep -oE '[0-9]+')
AFTER=$("$TAPUT" info rem.tap 2>/dev/null | grep Blocks | grep -oE '[0-9]+')
[ "$AFTER" -lt "$BEFORE" ]
ok "remove reduces the block count" $?

#---------------------------------------------------------------------------
echo "---------------------------------------------"
echo "PASS: $PASS  FAIL: $FAIL"
[ "$FAIL" -eq 0 ]
