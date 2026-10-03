#!/bin/sh
# End-to-end tests of a gzip binary against reference implementations.
# Usage: sh test/cli.sh ./gzip
# Set SLOW=1 to include a >4 GiB streaming test.
set -e

GZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
REF=${REF:-/usr/bin/gzip}
LIBDEFLATE=${LIBDEFLATE:-$(command -v libdeflate-gzip || true)}
tmp=$(mktemp -d)
trap 'cd / && rm -rf "$tmp"' EXIT
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

# The program name selects a default mode
ext=
case "$GZIP" in *.exe) ext=.exe;; esac
mkdir names
for n in gunzip zcat gzcat; do
    ln -s "$GZIP" names/$n$ext 2>/dev/null || cp "$GZIP" names/$n$ext
done
"$GZIP" -c text >n.gz
names/gunzip$ext -c n.gz | cmp -s - text || fail "gunzip -c"
names/zcat$ext n.gz | cmp -s - text || fail "zcat"
names/gzcat$ext <n.gz | cmp -s - text || fail "gzcat stdin"
[ -e n.gz ] || fail "zcat removed its input"
names/gunzip$ext n.gz
[ -e n ] && [ ! -e n.gz ] && cmp -s n text || fail "gunzip in place"

# Metadata: timestamps everywhere, permissions where they mean something
windows=
[ "$(uname -s)" = Windows_NT ] && windows=1
printf 'meta\n' >meta
touch -t 200102030405 meta ref
[ -z "$windows" ] && chmod 640 meta
"$GZIP" meta
[ meta.gz -nt ref ] || [ meta.gz -ot ref ] && fail "mtime not preserved"
if [ -z "$windows" ]; then
    [ "$(ls -l meta.gz | cut -c1-10)" = "-rw-r-----" ] ||
        fail "mode not preserved: $(ls -l meta.gz | cut -c1-10)"
    chmod 600 meta.gz
    "$GZIP" -d meta.gz
    [ "$(ls -l meta | cut -c1-10)" = "-rw-------" ] ||
        fail "mode not preserved on decompress"
else
    "$GZIP" -d meta.gz
fi
[ meta -nt ref ] || [ meta -ot ref ] && fail "mtime not preserved on decompress"

# Sub-second timestamps, where stat can show them
mtime() { stat -c %y "$1" 2>/dev/null || stat -f %Fm "$1"; }
printf 'fresh\n' >fresh
want=$(mtime fresh)
"$GZIP" fresh
[ "$(mtime fresh.gz)" = "$want" ] || fail "mtime $(mtime fresh.gz) != $want"
"$GZIP" -d fresh.gz
[ "$(mtime fresh)" = "$want" ] || fail "mtime lost on decompress"

# Symbolic links are skipped in place unless forced
printf 'target\n' >target
if ln -s target slink 2>/dev/null; then
    expect_status 2 "$GZIP" slink
    [ -e slink ] && [ ! -e slink.gz ] || fail "symlink compressed in place"
    "$GZIP" -c slink | "$GZIP" -dc | cmp -s - target || fail "-c symlink"
    "$GZIP" -f slink
    [ ! -e slink ] && [ -e slink.gz ] && [ -e target ] || fail "-f symlink"
fi

# Hard-linked files likewise
printf 'linked\n' >hard1
if ln hard1 hard2 2>/dev/null; then
    expect_status 2 "$GZIP" hard1
    [ -e hard1 ] && [ ! -e hard1.gz ] || fail "hard link compressed"
    "$GZIP" -f hard1
    [ -e hard1.gz ] && [ -e hard2 ] || fail "-f hard link"
fi

# FIFOs are never replaced, and checking one must not block
if command -v mkfifo >/dev/null && mkfifo fifo 2>/dev/null; then
    expect_status 2 "$GZIP" fifo
    expect_status 2 "$GZIP" -f fifo
    [ ! -e fifo.gz ] || fail "fifo compressed in place"
    printf 'piped\n' >fifo &
    "$GZIP" -c fifo | "$GZIP" -dc | grep -q piped || fail "-c fifo"
    wait
fi

# Write errors
if [ -w /dev/full ]; then
    set +e
    "$GZIP" -c text >/dev/full 2>/dev/null
    st=$?
    set -e
    [ $st = 1 ] || fail "write to /dev/full: status $st"
fi

# An interrupted in-place operation leaves no partial output. A
# non-interactive shell starts background jobs ignoring SIGINT, which
# gzip honors (as under nohup), so use SIGTERM.
# (busybox-w32 cannot reliably terminate a background job, so Windows is
# tested separately with Stop-Process.)
if [ -z "$windows" ]; then
head -c 300000000 /dev/urandom >big
"$GZIP" -9 big &
pid=$!
sleep 1
kill -TERM $pid 2>/dev/null || true
set +e
wait $pid
st=$?
set -e
if [ $st -ne 0 ]; then  # otherwise it finished first; nothing to check
    [ -e big ] || fail "input lost on interrupt"
    [ ! -e big.gz ] || fail "partial output left on interrupt"
fi
rm -f big big.gz
fi

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
