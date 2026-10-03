#!/bin/sh
# End-to-end tests of a gzip binary against reference implementations.
# Usage: sh test/cli.sh ./gzip
# Set SLOW=1 to include a >4 GiB streaming test.
set -e

GZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
REF=${REF:-/usr/bin/gzip}
LIBDEFLATE=${LIBDEFLATE:-$(command -v libdeflate-gzip || true)}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

expect_status() {
    want=$1
    shift
    set +e
    "$@" >/dev/null 2>&1
    got=$?
    set -e
    [ "$got" = "$want" ] || fail "expected status $want, got $got: $*"
}

# Inputs
: >empty
printf x >one
head -c 3000000 /dev/urandom >random
head -c 3000000 /dev/zero >zeros
cat /usr/share/dict/words /usr/share/dict/words >text 2>/dev/null ||
    awk 'BEGIN{for(i=0;i<300000;i++)print i, i*i, "lorem ipsum"}' >text
cp "$GZIP" binary

for f in empty one random zeros text binary; do
    for level in 1 2 3 4 5 6 7 8 9; do
        "$GZIP" -$level -c $f >$f.gz
        "$REF" -dc <$f.gz | cmp -s - $f || fail "$REF -d of $f at -$level"
        "$GZIP" -dc $f.gz | cmp -s - $f || fail "self -d of $f at -$level"
        if [ -n "$LIBDEFLATE" ]; then
            "$LIBDEFLATE" -dc $f.gz | cmp -s - $f ||
                fail "libdeflate -d of $f at -$level"
        fi
    done
    for level in 1 6 9; do
        "$REF" -$level -c <$f | "$GZIP" -dc | cmp -s - $f ||
            fail "self -d of $REF -$level $f"
    done
    if [ -n "$LIBDEFLATE" ]; then
        for level in 1 6 9 12; do
            "$LIBDEFLATE" -$level -c $f | "$GZIP" -dc | cmp -s - $f ||
                fail "self -d of libdeflate -$level $f"
        done
    fi
done

# In-place compression and decompression
cp text t
"$GZIP" t
[ ! -e t ] && [ -e t.gz ] || fail "in-place compress"
"$GZIP" -d t.gz
[ -e t ] && [ ! -e t.gz ] || fail "in-place decompress"
cmp -s t text || fail "in-place round trip"

# Refuse to overwrite without -f
cp text u
"$GZIP" -k u
expect_status 2 "$GZIP" u
[ -e u ] || fail "input removed after refusal"
"$GZIP" -f u
[ ! -e u ] || fail "-f did not compress"

# Multiple members
"$GZIP" -c one >m1.gz
"$GZIP" -c text >m2.gz
cat m1.gz m2.gz | "$GZIP" -dc >m.out
cat one text | cmp -s - m.out || fail "multi-member"

# Trailing garbage is a warning; corruption is an error
{ cat m1.gz; printf junk; } >tg.gz
expect_status 2 "$GZIP" -dc tg.gz
"$GZIP" -c text >c.gz
size=$(wc -c <c.gz)
head -c $((size - 4)) c.gz >trunc.gz
expect_status 1 "$GZIP" -t trunc.gz
expect_status 1 "$GZIP" -dc random
expect_status 0 "$GZIP" -t c.gz

# Corrupt output is removed, input kept
cp trunc.gz bad.gz
expect_status 1 "$GZIP" -d bad.gz
[ -e bad.gz ] && [ ! -e bad ] || fail "cleanup after failed decompress"

# Directories are skipped with a warning
mkdir d
expect_status 2 "$GZIP" d

if [ -n "$SLOW" ]; then
    # 5 GiB stream: exercises 64-bit offsets and ISIZE wraparound
    gen() {
        yes 'The quick brown fox jumps over the lazy dog 0123456789' |
            head -c 5368709120
    }
    gen | "$GZIP" -1 >big.gz
    "$REF" -t big.gz || fail "$REF -t of 5 GiB stream"
    "$GZIP" -dc big.gz | cksum >big.ours
    gen | cksum >big.want
    cmp -s big.ours big.want || fail "5 GiB round trip"
fi

echo "cli tests pass"
