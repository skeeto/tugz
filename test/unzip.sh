#!/bin/sh
# End-to-end tests of an unzip binary's listing and testing modes (-l,
# -v, -t, -p, -c, -z), comparing its status, standard output, and
# standard error with Info-ZIP's UnZip, REF, on archives from Python
# (test/unzipcraft.py, via uv when available), printf, tugz's zip
# (TUGZ_ZIP), and Info-ZIP's zip if present. Where tugz departs from
# UnZip, as documented, its output is checked on its own.
# Usage: REF=/usr/bin/unzip sh test/unzip.sh ./unzip
set -e

unset UNZIP UNZIPOPT ZIPOPT ZIP  # options from the environment
export TZ=UTC LC_ALL=C           # listings' times, and REF's names
U=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
REF=${REF:-unzip}
# Info-ZIP's, and not another that mentions it, as tugz's own does
if ! "$REF" -v 2>/dev/null | head -n 1 | grep -q '^UnZip .*Info-ZIP'; then
    echo "unzip.sh: REF ($REF) is not Info-ZIP's unzip" >&2
    exit 1
fi
REF=$(command -v "$REF")
# Known differences of REF's builds: Apple's names entries wrongly under
# -c; Debian's has Unicode support and finds overlapped components
apple=0
debian=0
"$REF" -v | grep -q Apple && apple=1
"$REF" -v | grep -q 'by Debian' && debian=1
CRAFT=$(cd "$(dirname "$0")" && pwd)/unzipcraft.py
if command -v uv >/dev/null 2>&1; then
    PY="uv run --no-project python3"
elif command -v python3 >/dev/null 2>&1; then
    PY=python3
else
    PY=
fi
if [ -n "$TUGZ_ZIP" ]; then
    TUGZ_ZIP=$(cd "$(dirname "$TUGZ_ZIP")" && pwd)/$(basename "$TUGZ_ZIP")
fi
version=$(sed -n 's/^#define TUGZ_VERSION "\(.*\)"$/\1/p' \
              "$(dirname "$0")/../src/base.c")
tmp=$(mktemp -d)
trap 'cd / && rm -rf "$tmp"' EXIT
cd "$tmp"

# A failure is reported on the original standard error, and marked, so
# that one within a pipeline or $(...), whose status no one sees, still
# fails the run at its end
exec 9>&2
fail() {
    echo "FAIL: $*" >&9
    : >"$tmp/FAILED"
    exit 1
}

# A sanitizer's report exits 99, a status unzip never gives
export ASAN_OPTIONS="exitcode=99${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export UBSAN_OPTIONS="exitcode=99${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"

# Run a command, with the caller's redirections, which must exit with a
# status, exactly: not with any failure, as a crash also is.
exits() {
    want=$1
    shift
    set +e
    "$@"
    got=$?
    set -e
    [ "$got" = "$want" ] || fail "expected status $want, got $got: $*"
}

# Run ours and REF on the same arguments, leaving their status, output,
# and errors in ours.* and ref.*, REF's dates made ISO (YYYY-MM-DD), as
# tugz lists them, rather than macOS's MM-DD-YYYY.
both() {
    set +e
    "$U" "$@" >ours.out 2>ours.err </dev/null
    echo $? >ours.st
    "$REF" "$@" >ref.raw 2>ref.err </dev/null
    echo $? >ref.st
    set -e
    [ "$(cat ours.st)" != 99 ] || fail "sanitizer: $*: $(cat ours.err)"
    sed 's/\([0-9][0-9]\)-\([0-9][0-9]\)-\([0-9][0-9][0-9][0-9]\) /\3-\1-\2 /' \
        ref.raw >ref.out
}

# The same status, output, and errors as REF's.
same() {
    both "$@"
    cmp -s ours.st ref.st ||
        fail "status $(cat ours.st), REF's $(cat ref.st): $*"
    cmp -s ours.out ref.out ||
        fail "output differs from REF's: $*: $(diff ref.out ours.out)"
    cmp -s ours.err ref.err ||
        fail "errors differ from REF's: $*: $(diff ref.err ours.err)"
}

# As same, but for the usage that follows an error, which is tugz's own:
# the errors before it, and whether it follows.
same_head() {
    both "$@"
    cmp -s ours.st ref.st ||
        fail "status $(cat ours.st), REF's $(cat ref.st): $*"
    for f in out err; do  # (under -t, errors go to standard output)
        sed '/^UnZip [0-9]/,$d' ref.$f >ref.pre
        sed '/^tugz unzip /,$d' ours.$f >ours.pre
        cmp -s ref.pre ours.pre ||
            fail "$f differs from REF's: $*: $(diff ref.pre ours.pre)"
        [ "$(grep -c '^UnZip [0-9]' ref.$f)" = \
          "$(grep -c '^tugz unzip ' ours.$f)" ] ||
            fail "usage, or not, unlike REF: $*: $(cat ours.$f)"
    done
}

# Our status, output, and errors exactly: status, a file of the expected
# output, a file of the expected errors, then the arguments.
ours() {
    want=$1
    wout=$2
    werr=$3
    shift 3
    set +e
    "$U" "$@" >ours.out 2>ours.err </dev/null
    got=$?
    set -e
    [ "$got" = "$want" ] ||
        fail "expected status $want, got $got: $*: $(cat ours.err)"
    cmp -s "$wout" ours.out ||
        fail "output: $*: $(diff "$wout" ours.out)"
    cmp -s "$werr" ours.err ||
        fail "errors: $*: $(diff "$werr" ours.err)"
}

: >none

# ---- Options --------------------------------------------------------

# Usage: alone to standard output, as after an error to standard error;
# -v alone gives tugz's version in a line
ours 0 ours.out none
head -n 1 ours.out | grep -q "^tugz unzip $version, a subset of Info-ZIP" ||
    fail "usage: $(cat ours.out)"
cp ours.out usage
ours 0 usage none -h
ours 0 usage none -q -h
"$U" -hh >hh 2>&1 || fail "-hh"
grep -q busybox hh && head -n 1 hh | grep -q "^tugz unzip" ||
    fail "-hh: $(cat hh)"
printf 'tugz unzip %s, a subset of Info-ZIP UnZip 6.0\n' "$version" >want
ours 0 want none -v
ours 0 want none -qv
ours 0 want none -v -q -g  # as UnZip, despite an unknown option
same_head -g x.zip
same_head -l            # options, but no archive
same_head -q

# Options UnZip has that tugz refuses, each with status 10
for opt in a aa B E F i I J L M N O P Q s S T U UU W Y Z '$' : '^' 2 /; do
    printf 'error:  -%c option not supported\n' "$opt" >want
    ours 10 none want -$opt x.zip
done

# An empty archive, from printf
printf 'PK\005\006\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' \
    >printf.zip
same -l printf.zip
same -t printf.zip

# Inputs for tugz's and Info-ZIP's zip
mkdir -p tree/sub
printf 'hello hello hello hello\n' >tree/a.txt
printf b >tree/b.txt
: >tree/empty
awk 'BEGIN{for(i=0;i<5000;i++)print i, i*i, "lorem ipsum"}' >tree/sub/text
touch -t 202001020304.05 tree/a.txt tree/b.txt tree/empty tree/sub/text

for zip in "$TUGZ_ZIP" "$(command -v zip || true)"; do
    [ -n "$zip" ] || continue
    rm -f z.zip
    (unset ZIP; "$zip" -qr z.zip tree) || fail "$zip -r"
    if [ "$zip" != "$TUGZ_ZIP" ]; then  # which has no -z
        printf 'An archive comment' | "$zip" -qz z.zip || fail "$zip -z"
    fi
    for mode in -l -v -t -p -z -lq -lqq -vq -tq -tqq -pq -c -cq; do
        [ $apple = 1 ] && [ "$mode" = -c ] && continue  # Apple's names
        same $mode z.zip
        same $mode z.zip 'tree/*.txt'
        same $mode z.zip -x 'tree/sub/*'
    done
    same -t z.zip tree/a.txt nomatch          # caution, status 11
    same -l z.zip nomatch                     # 11, silently
    same -t z.zip -x nomatch                  # caution, status 0
    same -tC z.zip TREE/A.TXT                 # -C ignores case
    same -t z.zip TREE/A.TXT                  # 11
    same -t z.zip 'tree/[ab].txt'             # sets
    same -t z.zip 'tree/[!a].txt'
    same -t z.zip 'tree/a\.txt'               # escapes
    same -t z
    cp z.zip up.ZIP                           # (or up.zip, ignoring case)
    same -t up
    rm up.ZIP
    same -l -d somewhere z.zip                # -d ignored, with caution
    same -l z.zip -d somewhere
    same -l z.zip -dsomewhere
    same_head -l -d                           # -d needs a directory
    same_head -l z.zip -d
    same_head -l -d a -d b z.zip              # -d twice
    same_head -lt z.zip                       # -l with -t tests
    same_head -ct z.zip
    same_head -tu z.zip
    same_head -fn z.zip
    same -t -n -o z.zip                       # -o ignored, with caution
    same --q -l z.zip                         # negations
    same -q-q -l z.zip
    same -qq--q -l z.zip
    same -t -x z.zip                          # -x before the archive
    same -bee -l z.zip                        # -b, -e: nothing

    # The environment: UNZIP, or else UNZIPOPT, before the arguments
    for var in UNZIP UNZIPOPT; do
        export $var="-qq"
        same -l z.zip
        same --q -l z.zip
        unset $var
    done
    # (Assignments before a function may outlive it: export, then unset)
    export UNZIP=" " UNZIPOPT=-q              # UNZIP has none
    same -l z.zip
    export UNZIP=-q UNZIPOPT=-qq
    same -l z.zip
    unset UNZIPOPT
    export UNZIP='-l "z.zip"'
    same
    same tree/a.txt
    export UNZIP='-l "z.zip" tree/a.txt'
    same -x tree/a.txt
    export UNZIP=-v
    same_head -q                              # options, no archive
    unset UNZIP
done

# tugz's own: options after the archive, as busybox takes them
if [ -n "$TUGZ_ZIP" ] || command -v zip >/dev/null 2>&1; then
    "$U" -l z.zip >want 2>&1
    ours 0 want none z.zip -l
    "$U" -lq z.zip >want 2>&1
    ours 0 want none z.zip -l -q
    ours 0 want none z.zip -l--qq
    "$U" -t z.zip tree/a.txt -x tree/b.txt >want 2>&1
    ours 0 want none z.zip tree/a.txt -x tree/b.txt -t
    "$U" -lq z.zip tree/a.txt >want 2>&1
    ours 0 want none z.zip -q tree/a.txt -l    # among members
fi

# ---- Archives -------------------------------------------------------

same -l nothere                               # 9, each name tried
: >empty.file
same -l empty.file
mkdir adir adir.zip
same -l adir                                  # a directory, then adir.zip
same -t adir.zip
printf 'not a zip archive\n' >notzip
same -t notzip
same -tq notzip
same -tqq notzip
same -tqqq notzip
cp notzip notzip.exe
chmod +x notzip.exe
same -l notzip.exe                            # perhaps an executable

if [ -z "$PY" ]; then
    echo "unzip.sh: skipping crafted archives, which need Python" >&2
    [ ! -e "$tmp/FAILED" ] || exit 1
    exit 0
fi
mkdir craft
(cd craft && $PY "$CRAFT") || fail "unzipcraft.py"
cd craft
: >none

# Archives that tugz reads as UnZip does, in every mode
for z in basic nocomment sfx sfxok zip64 desc empty emptycomment emptysfx \
         multi many badcrc baddata truncdata storedsize localname badextra \
         truncated badcdoff badheader badlocal; do
    for mode in -l -v -t -tq -tqq -p -pq -z -zq -c -cq; do
        [ $apple = 1 ] && [ "$mode" = -c ] && continue  # Apple's names
        same $mode $z.zip
    done
done
same -l basic                                 # with .zip
same -t basic dir/text.txt
same -l methods.zip
same -v methods.zip
same -t overrun.zip

# Wildcard archive names: each match, in name order, then a summary,
# leaving out names starting with a dot unless the pattern does
mkdir w
cp basic.zip w/a.zip
same -l 'w/*.zip'                             # one match, no summary
cp basic.zip w/.dot.zip
same -l 'w/*.zip'
same -l 'w/.*.zip'
same -l 'w/[a].zip'
same -l 'w/?.zip'
same -l 'nomatch*'                            # No zipfiles found.
cp sfx.zip w/b.zip
cp notzip w/c.zip
mkdir w/d.zip
cp badheader.zip w/e.zip
for mode in -l -t -tq -tqq -tqqq; do
    both $mode 'w/*.zip'
    cmp -s ours.st ref.st || fail "status: $mode w/*.zip"
    # Archives come in name order, rather than the directory's: compare
    # the summary (none with -qqq), from standard output under -t
    f=err
    [ $mode = -l ] || f=out
    tail -n 5 ours.$f >ours.sum
    tail -n 5 ref.$f >ref.sum
    [ $mode = -tqqq ] || cmp -s ours.sum ref.sum ||
        fail "summary: $mode w/*.zip: $(diff ref.sum ours.sum)"
    # and the rest, but for the blank lines between archives
    sort ours.out ours.err | grep -v '^$' >ours.all
    sort ref.out ref.err | grep -v '^$' >ref.all
    cmp -s ours.all ref.all || fail "lines: $mode w/*.zip"
done
cat >want <<EOF
Archive:  w/a.zip
The archive comment.
    testing: dir/                     OK
    testing: dir/text.txt             OK
    testing: dir/stored.txt           OK
    testing: empty                    OK
    testing: ut.txt                   OK
No errors detected in compressed data of w/a.zip.

Archive:  w/b.zip
EOF
"$U" -t 'w/*.zip' >ours.out 2>&1 </dev/null || true
head -n 10 ours.out | cmp -s - want || fail "w/*.zip order: $(cat ours.out)"

# Standard input as the archive, busybox's: a file read in place, or a
# pipe spooled into memory
for mode in -l -v -t -p -z; do
    "$U" $mode basic.zip >want 2>&1
    sed 's/basic\.zip/-/' want >want.stdin
    exits 0 "$U" $mode - <basic.zip >got 2>&1
    cmp -s want.stdin got || fail "$mode - <file: $(diff want.stdin got)"
    cat basic.zip | { exits 0 "$U" $mode - >got 2>&1; }
    cmp -s want.stdin got || fail "$mode - pipe: $(diff want.stdin got)"
done
"$U" -p basic.zip dir/text.txt >want
cat sfx.zip | { exits 1 "$U" -p - dir/text.txt >got 2>/dev/null; }
cmp -s want got || fail "-p - from a pipe"
cat ../notzip | { exits 9 "$U" -t - >got 2>&1; }
grep -q 'End-of-central-directory signature not found' got || fail "- notzip"
: | { exits 9 "$U" -t - >/dev/null 2>&1; }

# Entries skipped, as by an UnZip without decryption or the methods:
# encrypted, Deflate64, and others (UnZip builds that have them, as REF,
# decode them)
cat >want <<EOF
Archive:  methods.zip
   skipping: enc.txt                 encrypted (not supported)
   skipping: d64.bin                 \`deflate64' method not supported
   skipping: bzip2.bin               \`bzip2' method not supported
   skipping: lzma.bin                \`LZMA' method not supported
   skipping: shrunk.bin              \`shrink' method not supported
   skipping: aes.bin                 unsupported compression method 99
   skipping: new.bin                 need PK compat. v6.3 (can do v4.5)
    testing: ok.txt                   OK
    testing: last.txt                 OK
No errors detected in methods.zip for the 2 files tested.
7 files skipped because of unsupported compression or encoding.
EOF
ours 81 want none -t methods.zip
grep '^   skipping' want >want.err
"$U" -p methods.zip ok.txt last.txt >want.out
ours 81 want.out none -p methods.zip           # -p says nothing
"$U" -c methods.zip ok.txt last.txt >want.out
ours 81 want.out want.err -c methods.zip
cat >want <<EOF
Archive:  encrypted.zip
   skipping: enc.txt                 encrypted (not supported)
Caution:  zero files tested in encrypted.zip.
1 file skipped because of unsupported compression or encoding.
EOF
ours 81 want none -t encrypted.zip
printf 'Archive:  encrypted.zip\n' >want
printf '   skipping: enc.txt                 encrypted (not supported)\n' >want.err
ours 81 want want.err -c encrypted.zip
ours 81 none none -pq encrypted.zip
printf '   skipping: d64.bin                 `deflate64'"'"' method not supported\n' >want.err
ours 81 none none -p deflate64.zip
printf 'Archive:  deflate64.zip\n' >want
ours 81 want want.err -c deflate64.zip
ours 81 none none -qqt deflate64.zip

# Overlapped components, a zip bomb's: found before any entry is read
# (Debian's UnZip finds them as it reads the second), with Debian's
# message and status; data reaching into the central directory too
# (which Debian's does not check)
for z in overlap inner overlapcd; do
    printf 'Archive:  %s.zip\nerror: invalid zip file with overlapped components (possible zip bomb)\n' $z >want
    ours 12 want none -t $z.zip
    printf 'error: invalid zip file with overlapped components (possible zip bomb)\n' >want
    ours 12 want none -tq $z.zip
    ours 12 none want -p $z.zip
    if [ $debian = 1 ] && [ $z != overlapcd ]; then
        same -tq $z.zip
        same -pq $z.zip 'b*'
    fi
    same -l $z.zip                            # listing reads no data
done

# Output beyond an entry's size is not written: an overrun, which UnZip
# writes out before finding the CRC wrong
printf 'hello hell' >want
"$U" -p overrun.zip >got 2>got.err && fail "-p overrun.zip: status 0"
cmp -s want got || fail "-p overrun.zip output"

# Names: UTF-8, by flag or Unicode path field, as Debian's UnZip shows
# them (as UTF-8), and control characters as ^X
cat >want <<EOF
Archive:  names.zip
  Length      Date    Time    Name
---------  ---------- -----   ----
        5  2020-01-02 03:04   café.txt
        6  2020-01-02 03:04   café.txt
        4  2020-01-02 03:04   ctl^A^[[1mx.txt
        4  2020-01-02 03:04   dos\\path.txt
---------                     -------
       19                     4 files
EOF
ours 0 want none -l names.zip
"$U" -t names.zip >got 2>&1 && fail "-t names.zip: status 0"
grep -q 'mismatching "local" filename (cafe.txt)' got ||
    fail "-t names.zip: $(cat got)"
printf 'utf8\nupath\n' >want  # each matched by its Unicode name
sed -n '/mismatching/{N;p;}' got >want.err
ours 1 want want.err -pq names.zip 'caf??.txt'
printf 'caution: filename not matched:  cafe.txt\n' >want.err
ours 11 none want.err -pq names.zip cafe.txt

# Comments shown as UnZip shows them, without carriage returns, escape
# as ^[, but other control characters as ^X too (where UnZip writes
# them raw), and only up to a NUL
cat >want <<EOF
Archive:  comments.zip
line one
line^[[31m two^G
EOF
ours 0 want none -z comments.zip
cat >want <<EOF
Archive:  comments.zip
line one
line^[[31m two^G
  Length      Date    Time    Name
---------  ---------- -----   ----
        2  2020-01-02 03:04   c.txt
tab	here^[[1m bold
---------                     -------
        2                     1 file
EOF
ours 0 want none -l comments.zip

[ ! -e "$tmp/FAILED" ] || exit 1
