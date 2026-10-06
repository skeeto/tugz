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

# ...and failing to replace one with -f is an error, as in GNU gzip
printf d >dd
mkdir dd.gz
expect_status 2 "$GZIP" -k dd
expect_status 1 "$GZIP" -kf dd

# GNU gzip's other suffixes, in any case, where .taz stands for .tar
for s in .z -gz _Z .TAZ; do
    printf x | "$GZIP" >sfx$s
    "$GZIP" -d sfx$s
    case $s in .TAZ) out=sfx.tar;; *) out=sfx;; esac
    cmp -s $out one && [ ! -e sfx$s ] || fail "-d of a $s suffix"
    rm $out
done

# As there, decompressing or testing a missing name without a suffix
# tries it with suffixes (.gz, .z, -z, .Z), so that zcat foo reads foo.gz
printf x | "$GZIP" >miss-z
"$GZIP" -dc miss | cmp -s - one || fail "-dc of NAME for NAME-z"
expect_status 0 "$GZIP" -t miss
"$GZIP" -d miss
[ -e miss ] && [ ! -e miss-z ] && cmp -s miss one || fail "-d of NAME-z"
expect_status 1 "$GZIP" -dq gone

# ...and a file with one is left alone, which is no failure, unless
# forced, but a missing one is an error
printf data >has.tgz
expect_status 0 "$GZIP" has.tgz
[ -e has.tgz ] && [ ! -e has.tgz.gz ] || fail "compressed a .tgz"
"$GZIP" -f has.tgz
[ ! -e has.tgz ] && [ -e has.tgz.gz ] || fail "-f did not compress a .tgz"
expect_status 1 "$GZIP" has.tgz

# Headers have no name or time, as GNU gzip's under -n, which does nothing
"$GZIP" -c text >nn.gz
"$GZIP" -nc text | cmp -s - nn.gz || fail "-n"
"$GZIP" --no-name -c text | cmp -s - nn.gz || fail "--no-name"
expect_status 1 "$GZIP" -Nc text

# Multiple members
"$GZIP" -c one >m1.gz
"$GZIP" -c text >m2.gz
cat m1.gz m2.gz | "$GZIP" -dc >m.out
cat one text | cmp -s - m.out || fail "multi-member"

# Files in one run share an encoder yet compress as each does alone
"$GZIP" -c text one empty binary one >m3.gz
for f in text one empty binary one; do "$GZIP" -c $f; done |
    cmp -s - m3.gz || fail "several files in one run"

# Trailing garbage is a warning; corruption is an error
{ cat m1.gz; printf junk; } >tg.gz
expect_status 2 "$GZIP" -dc tg.gz
"$GZIP" -c text >c.gz
size=$(wc -c <c.gz)
head -c $((size - 4)) c.gz >trunc.gz
expect_status 1 "$GZIP" -t trunc.gz
expect_status 1 "$GZIP" -dc random
expect_status 0 "$GZIP" -t c.gz

# As in GNU gzip, zero bytes after a member are padding, while a lone
# byte other than zero may begin one, which is then truncated
{ cat m1.gz; head -c 1000 /dev/zero; } >pad.gz
expect_status 0 "$GZIP" -t pad.gz
"$GZIP" -dc pad.gz | cmp -s - one || fail "zero padding"
{ cat m1.gz; head -c 10 /dev/zero; printf x; } >padx.gz
expect_status 2 "$GZIP" -t padx.gz
{ cat m1.gz; printf '\037'; } >cut.gz
expect_status 1 "$GZIP" -t cut.gz
cp cut.gz cut1.gz
expect_status 1 "$GZIP" -d cut1.gz
[ -e cut1.gz ] && [ ! -e cut1 ] || fail "cleanup after a cut member"

# Forced, as in GNU gzip (zcat -f), data that is not gzip, whole or after
# a member, passes through to standard output, or passes a test, but in
# place it is still an error
printf 'plain\n' >plain
{ cat plain one one; printf junk; cat plain; } >pass.want
"$GZIP" -dcf plain m1.gz tg.gz - <plain >pass.got || fail "-dcf status"
cmp -s pass.got pass.want || fail "-dcf pass-through"
expect_status 0 "$GZIP" -tf plain tg.gz
expect_status 1 "$GZIP" -dc plain
cp plain pl.gz
expect_status 1 "$GZIP" -df pl.gz
[ -e pl.gz ] && [ ! -e pl ] || fail "-df in place"

# ...and as there, the header is read before the output is created, so
# an existing output survives input that is not gzip, even forced
printf 'keep me\n' >keep
for h in '' '\037' '\037\213' '\037\213\007\000\000\000\000\000\000\003'; do
    printf "$h" >hd.gz
    cp hd.gz hd.want
    for opts in -df -d; do
        cp keep hd
        expect_status 1 "$GZIP" $opts hd.gz
        cmp -s hd keep && cmp -s hd.gz hd.want || fail "$opts over hd: '$h'"
    done
done
cp plain hd.gz
cp keep hd
expect_status 1 "$GZIP" -df hd.gz
cmp -s hd keep && cmp -s hd.gz plain || fail "-df over hd: plain"

# Corrupt output is removed, input kept
cp trunc.gz bad.gz
expect_status 1 "$GZIP" -d bad.gz
[ -e bad.gz ] && [ ! -e bad ] || fail "cleanup after failed decompress"

# Directories are skipped with a warning
mkdir d
expect_status 2 "$GZIP" d

# Long options, help and version on standard output, end of options
"$GZIP" -9 -c text >o9.gz
"$GZIP" -1 -c text >o1.gz
"$GZIP" --best --stdout text | cmp -s - o9.gz || fail "--best --stdout"
"$GZIP" --fast --to-stdout text | cmp -s - o1.gz || fail "--fast"
"$GZIP" --decompress --stdout o9.gz | cmp -s - text || fail "--decompress"
"$GZIP" --uncompress -c o1.gz | cmp -s - text || fail "--uncompress"
expect_status 0 "$GZIP" --test o9.gz
cp one kk
"$GZIP" --keep kk
[ -e kk ] && [ -e kk.gz ] || fail "--keep"
expect_status 2 "$GZIP" -k kk
"$GZIP" --force --keep kk || fail "--force"
expect_status 1 "$GZIP" --bogus kk
expect_status 1 "$GZIP" -x kk
for opt in -h --help -V --version; do
    "$GZIP" $opt >opt.out 2>opt.err || fail "$opt status"
    [ -s opt.out ] && [ ! -s opt.err ] || fail "$opt output"
done
printf data >-k
"$GZIP" -- -k
[ -e -k.gz ] && [ ! -e -k ] || fail "-- -k"

# -q silences warnings but not errors, and keeps the status
set +e
"$GZIP" -dcq tg.gz >/dev/null 2>q1.err
st1=$?
"$GZIP" --quiet -t tg.gz 2>q2.err
st2=$?
"$GZIP" -q gone 2>q3.err
st3=$?
set -e
[ $st1 = 2 ] && [ ! -s q1.err ] || fail "-q warning: $st1"
[ $st2 = 2 ] && [ ! -s q2.err ] || fail "--quiet warning: $st2"
[ $st3 = 1 ] && [ -s q3.err ] || fail "-q error: $st3"

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
names/zcat$ext n | cmp -s - text || fail "zcat of NAME for NAME.gz"
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

# ...and the access time an input had before it was read, as in GNU gzip
atime() { stat -c %X "$1" 2>/dev/null || stat -f %a "$1"; }
printf 'aged\n' >aged
if touch -a -t 200102030405 aged 2>/dev/null; then
    want=$(atime aged)
    "$GZIP" aged
    [ "$(atime aged.gz)" = "$want" ] || fail "atime $(atime aged.gz) != $want"
    "$GZIP" -d aged.gz
    [ "$(atime aged)" = "$want" ] || fail "atime lost on decompress"
fi

# Symbolic links are refused in place unless forced, an error as in GNU gzip
printf 'target\n' >target
if ln -s target slink 2>/dev/null; then
    expect_status 1 "$GZIP" slink
    [ -e slink ] && [ ! -e slink.gz ] || fail "symlink compressed in place"
    "$GZIP" -c slink | "$GZIP" -dc | cmp -s - target || fail "-c symlink"
    "$GZIP" -f slink
    [ ! -e slink ] && [ -e slink.gz ] && [ -e target ] || fail "-f symlink"
fi

# Hard-linked files likewise, but with a warning, as there
printf 'linked\n' >hard1
if ln hard1 hard2 2>/dev/null; then
    expect_status 2 "$GZIP" hard1
    [ -e hard1 ] && [ ! -e hard1.gz ] || fail "hard link compressed"
    "$GZIP" -f hard1
    [ -e hard1.gz ] && [ -e hard2 ] || fail "-f hard link"
fi

# An input that cannot be removed (here a BSD user-immutable file) is
# left with its output and a warning, as in GNU gzip
printf 'stuck\n' >stuck
if chflags uchg stuck 2>/dev/null; then
    set +e
    "$GZIP" stuck 2>/dev/null
    st=$?
    set -e
    chflags nouchg stuck
    [ $st = 2 ] && [ -e stuck ] && [ -e stuck.gz ] ||
        fail "input not removed: status $st"
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

# Windows: a bare DOS device name is the device, as in other programs
if [ -n "$windows" ]; then
    "$GZIP" -c NUL | "$GZIP" -dc | cmp -s - empty || fail "-c NUL"
    expect_status 2 "$GZIP" NUL
fi

# Windows: a device path is resolved as Win32 resolves it, except one
# written exactly \\?\, which Win32 takes as it is
if [ -n "$windows" ]; then
    here=$(pwd)  # C:/...
    for p in "//?/$here/names/../one" "\\\\?/$here/./one" "//./$here//one"; do
        "$GZIP" -c "$p" | "$GZIP" -dc | cmp -s - one || fail "-c $p"
    done
    win=$(printf %s "$here" | tr / '\\')
    expect_status 0 "$GZIP" -c "\\\\?\\$win\\one"
    expect_status 1 "$GZIP" -c "\\\\?\\$win\\names\\..\\one"
    # ...but one missing its device, server, or share names nothing
    for p in "//?/" "//./" "//?/UNC/localhost/" "\\\\localhost\\" "//"; do
        expect_status 1 "$GZIP" -c "$p"
    done
fi

# Windows: -f replaces a read-only output, as unlinking it does on POSIX,
# though the file's other hard links stay read-only, and without -f it
# is refused
if [ -n "$windows" ]; then
    printf old >ro
    "$GZIP" -k ro
    cp ro.gz ro.old
    ln ro.gz ro.link
    attrib +r ro.gz
    printf new >ro
    expect_status 2 "$GZIP" -k ro
    cmp -s ro.gz ro.old || fail "read-only output replaced without -f"
    "$GZIP" -kf ro || fail "-f over a read-only output"
    "$GZIP" -dc ro.gz | cmp -s - ro || fail "-f read-only output contents"
    cmp -s ro.link ro.old || fail "-f read-only output: other link changed"
    if (: >>ro.link) 2>/dev/null; then
        fail "-f read-only output: other link writable"
    fi
    attrib -r ro.link
fi

# Windows: an output name that another process holds delete-pending, as
# gzip holds its own until done, is refused, an error under -f
if [ -n "$windows" ]; then
    printf p >pend
    cat >pend.cs <<'EOF'
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
public static class Pend {
    [DllImport("kernel32.dll")]
    static extern bool SetFileInformationByHandle(System.IntPtr h, int c,
                                                  ref byte discard, int n);
    public static FileStream Hold(string path) {
        FileStream f = new FileStream(path, FileMode.CreateNew,
            FileSystemRights.Write | FileSystemRights.Delete,
            FileShare.None, 1, FileOptions.None);
        byte discard = 1;
        SetFileInformationByHandle(f.SafeFileHandle.DangerousGetHandle(),
                                   4, ref discard, 1);  // FileDispositionInfo
        return f;
    }
}
EOF
    powershell -NoProfile -NonInteractive -Command "
        Add-Type -TypeDefinition (Get-Content -Raw pend.cs)
        \$f = [Pend]::Hold((Join-Path (Get-Location) pend.gz))
        Set-Content held ''
        for (\$i = 0; \$i -lt 600 -and !(Test-Path done); \$i++) {
            Start-Sleep -Milliseconds 50
        }
        \$f.Close()" &
    pid=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -e held ] && break; sleep 1; done
    set +e
    "$GZIP" -k pend 2>err1; st1=$?
    "$GZIP" -kf pend 2>err2; st2=$?
    "$GZIP" -kqf pend 2>err3; st3=$?
    set -e
    : >done
    wait $pid || true
    [ $st1 = 2 ] && grep -q 'already exists' err1 ||
        fail "delete-pending output: $st1 $(cat err1)"
    [ $st2 = 1 ] && grep -q '^gzip: pend.gz: Permission denied' err2 ||
        fail "-f delete-pending output: $st2 $(cat err2)"
    [ $st3 = 1 ] && [ -s err3 ] || fail "-qf delete-pending output: $st3"
    "$GZIP" -kf pend || fail "-f once no longer delete-pending"
fi

# As in GNU gzip, a read error ends the run, and leaves the stream
# unfinished rather than passing for all of the input (standard input
# here is open only for writing)
: >wo
for opts in -c -dc; do
    set +e
    "$GZIP" $opts - m1.gz 0>>wo >wo.out 2>wo.err
    st=$?
    set -e
    [ $st = 1 ] && [ ! -s wo.out ] && [ "$(grep -c . wo.err)" = 1 ] ||
        fail "$opts read error: status $st, $(wc -c <wo.out) bytes out"
done

# Write errors, which likewise end the run, naming the output
if [ -w /dev/full ]; then
    set +e
    "$GZIP" -c text text >/dev/full 2>full.err
    st=$?
    set -e
    [ $st = 1 ] || fail "write to /dev/full: status $st"
    [ "$(grep -c . full.err)" = 1 ] && grep -q '^gzip: stdout: ' full.err ||
        fail "write to /dev/full: $(cat full.err)"
fi

# ...but a closed pipe, which the default SIGPIPE ends quietly, is only a
# warning, as in GNU gzip where SIGPIPE is ignored, or (Windows) absent
for opts in -c -dc; do
    f=random
    [ $opts = -dc ] && f=random.gz
    (
        trap '' PIPE 2>/dev/null || true
        { st=0; "$GZIP" $opts $f $f 2>pipe.err || st=$?
          echo $st >pipe.st; } | head -c 10 >/dev/null
    )
    [ "$(cat pipe.st)" = 2 ] && [ "$(grep -c . pipe.err)" = 1 ] &&
        grep -q '^gzip: stdout: Broken pipe$' pipe.err ||
        fail "$opts to a closed pipe: $(cat pipe.st) $(cat pipe.err)"
done

# Failures give the system's reason, as in GNU gzip
for opts in '' -d; do
    set +e
    "$GZIP" $opts nothere 2>why.err
    st=$?
    set -e
    name=nothere
    [ -n "$opts" ] && name=nothere.gz
    [ $st = 1 ] && grep -q "^gzip: $name: No such file or directory\$" why.err ||
        fail "$opts nothere: $st $(cat why.err)"
done

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
wait $pid 2>/dev/null  # without the shell's notice of the signal
st=$?
set -e
if [ $st -ne 0 ]; then  # otherwise it finished first; nothing to check
    [ -e big ] || fail "input lost on interrupt"
    [ ! -e big.gz ] || fail "partial output left on interrupt"
fi
rm -f big big.gz

# So does exceeding the file size limit (SIGXFSZ). An inner shell
# reports the signal, so that its notice goes to /dev/null.
head -c 300000 /dev/urandom >big
st=$(sh -c 'ulimit -f 100; "$1" big; echo $?' sh "$GZIP" 2>/dev/null)
[ "$st" -ne 0 ] || fail "file size limit not reached"
[ -e big ] || fail "input lost at file size limit"
[ ! -e big.gz ] || fail "partial output left at file size limit"
rm -f big
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
