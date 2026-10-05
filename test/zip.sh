#!/bin/sh
# End-to-end tests of a zip binary, verified with unzip, zipinfo, and
# Python's zipfile (via uv when available).
# Usage: sh test/zip.sh ./zip
# Set SLOW=1 to include Zip64 tests: a 5 GiB file and 70,000 entries.
set -e

unset ZIPOPT ZIP  # options for zip, and ZIP unexported for the binary
ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
CHECK=$(cd "$(dirname "$0")" && pwd)/zipcheck.py
if command -v uv >/dev/null 2>&1; then
    PY="uv run --no-project python3"
elif command -v python3 >/dev/null 2>&1; then
    PY=python3
else
    PY=
fi
tmp=$(mktemp -d)
trap 'cd / && chmod -R u+rwx "$tmp" && rm -rf "$tmp"' EXIT
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

# Check an archive with two independent readers.
verify() {
    unzip -tqq "$1" >/dev/null 2>&1 || fail "unzip -t $1"
    if [ -n "$PY" ]; then
        $PY "$CHECK" "$1" >check.out 2>&1 || fail "zipfile $1: $(cat check.out)"
    fi
}

names() {
    zipinfo -1 "$1" 2>/dev/null || true
}

extract_same() {  # archive tree
    rm -rf x
    mkdir x
    (cd x && unzip -qq "../$1") || fail "unzip $1"
    diff -r "$2" "x/$2" >/dev/null || fail "contents of $1 differ from $2"
}

# Progress lines without the compression percentage
progress() {
    sed 's/ (deflated [0-9]*%)//; s/ (stored [0-9]*%)//' "$1"
}

# Inputs
mkdir -p tree/sub/deeper tree/.hidden
printf 'hello hello hello hello\n' >tree/a.txt
printf b >tree/b.txt
printf zz >tree/Z.txt
: >tree/empty
printf x >tree/one
head -c 300000 /dev/urandom >tree/sub/random
awk 'BEGIN{for(i=0;i<50000;i++)print i, i*i, "lorem ipsum"}' >tree/sub/deeper/text

# Recursion: byte-sorted names, directory entries, verified contents
"$ZIP" -qr t.zip tree
verify t.zip
names t.zip >got
cat >want <<EOF
tree/
tree/.hidden/
tree/Z.txt
tree/a.txt
tree/b.txt
tree/empty
tree/one
tree/sub/
tree/sub/deeper/
tree/sub/deeper/text
tree/sub/random
EOF
cmp -s got want || fail "recursive names: $(cat got)"
extract_same t.zip tree

# Every level round trips; -0 stores everything; small and empty files,
# and incompressible data, are stored at any level
for level in 0 1 2 3 4 5 6 7 8 9; do
    rm -f l.zip
    "$ZIP" -qr -$level l.zip tree
    verify l.zip
    extract_same l.zip tree
    if [ $level = 0 ]; then
        zipinfo l.zip | grep -q def && fail "-0 compressed something"
    fi
    zipinfo l.zip | grep 'tree/empty$' | grep -q stor || fail "empty -$level"
    zipinfo l.zip | grep 'tree/one$'   | grep -q stor || fail "one -$level"
    zipinfo l.zip | grep 'random$'     | grep -q stor || fail "random -$level"
    if [ $level != 0 ]; then
        zipinfo l.zip | grep 'text$' | grep -q def || fail "text -$level"
    fi
done

# Progress messages
"$ZIP" p.zip tree/a.txt tree/empty >out
progress out >got
printf '  adding: tree/a.txt\n  adding: tree/empty\n' >want
cmp -s got want || fail "progress: $(cat out)"
grep -q 'tree/empty (stored 0%)' out || fail "stored message: $(cat out)"
"$ZIP" -q q.zip tree/a.txt missing >out 2>&1
[ ! -s out ] || fail "-q not quiet: $(cat out)"

# Combined, long, and negated options, under Info-ZIP's names, which
# may be abbreviated
"$ZIP" -qX9r c1.zip tree
"$ZIP" --quiet --strip-extra --compress-9 --recurse-paths c2.zip tree
cmp -s c1.zip c2.zip || fail "long options differ from short"
"$ZIP" --qui --strip --compress-9 --recurse-path c2.zip tree
cmp -s c1.zip c2.zip || fail "abbreviated long options differ"
"$ZIP" -qX -X- -9r c3.zip tree
[ "$(wc -c <c3.zip)" -gt "$(wc -c <c1.zip)" ] || fail "-X- kept -X"
"$ZIP" -qX9 --strip-extra- -rp --paths c4.zip tree
cmp -s c3.zip c4.zip || fail "--strip-extra- or -p"
"$ZIP" -q --store c5.zip tree/a.txt
zipinfo c5.zip | grep -q stor || fail "--store compressed"
for opt in -q- -r- -d- -u- -f- -FS- -0- -9- -j- -nw- -@- -p- --quiet- \
           --delete- --exclude- --rec --compress --no-extra --store-only \
           --system-hidden --quiet=x -S -e --encr; do
    expect_status 16 "$ZIP" $opt c6.zip tree/a.txt
done
[ ! -e c6.zip ] || fail "an invalid option made an archive"
"$ZIP" -qr c7.zip tree -x-  # a list's value, not a negation
names c7.zip | grep -q tree/a.txt || fail "-x- excluded a.txt"

# Determinism: -X output depends only on the tree, and SOURCE_DATE_EPOCH
# makes it independent of the time zone
"$ZIP" -qX9r d1.zip tree
"$ZIP" -qX9r d2.zip tree
cmp -s d1.zip d2.zip || fail "-X output not deterministic"
SOURCE_DATE_EPOCH=1700000000 TZ=UTC "$ZIP" -qX9r e1.zip tree
SOURCE_DATE_EPOCH=1700000000 TZ=Asia/Tokyo "$ZIP" -qX9r e2.zip tree
cmp -s e1.zip e2.zip || fail "SOURCE_DATE_EPOCH output depends on TZ"
TZ=UTC touch -t 200001020304.05 tree/one
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX e3.zip tree/one tree/a.txt
zipinfo -T e3.zip | grep 'tree/one$' | grep -q 20000102.030406 ||
    fail "older time not kept (rounded up): $(zipinfo -T e3.zip)"
zipinfo -T e3.zip | grep 'tree/a.txt$' | grep -q 20231114.221320 ||
    fail "newer time not clamped: $(zipinfo -T e3.zip)"
expect_status 16 env SOURCE_DATE_EPOCH=soon "$ZIP" -q e4.zip tree/one

# Archive names: .zip appended only without an extension
"$ZIP" -q noext tree/a.txt
[ -f noext.zip ] || fail ".zip not appended"
"$ZIP" -q other.ext tree/a.txt
[ -f other.ext ] || fail "extension not kept"

# Path handling: ./ and leading / dropped, ../ kept, doubled / collapsed
(cd tree && "$ZIP" -q ../n.zip ./a.txt sub//random ../tree/b.txt "$tmp/tree/one")
names n.zip >got
printf '%s\n' a.txt sub/random ../tree/b.txt "${tmp#/}/tree/one" >want
cmp -s got want || fail "path names: $(cat got)"

# A leading //host/share/ is dropped, as Info-ZIP's Unix ex2in does
unc="/$tmp/tree/a.txt"
"$ZIP" -q unc.zip "$unc"
[ "$(names unc.zip)" = "${unc#//*/*/}" ] || fail "//host/share/: $(names unc.zip)"

# The same path reached twice is added once, as in Info-ZIP, while
# different paths for one name are an error
"$ZIP" -q dup1.zip tree/a.txt tree/a.txt tree//a.txt
[ "$(names dup1.zip)" = tree/a.txt ] || fail "same path twice: $(names dup1.zip)"
"$ZIP" -qr dup2.zip tree tree/a.txt tree/ tree/sub/deeper tree/sub
names dup2.zip >got
names t.zip >want
cmp -s got want || fail "overlapping paths: $(cat got)"
find tree | "$ZIP" -qr -@ dup3.zip
[ "$(names dup3.zip | wc -l)" = "$(wc -l <want)" ] || fail "find | zip -r@"
expect_status 16 "$ZIP" dup4.zip tree/a.txt ./tree/a.txt
"$ZIP" -j dup5.zip tree/a.txt tree/sub/../a.txt 2>out && fail "-j collision"
grep -q 'result of using -j' out || fail "-j collision: $(cat out)"
[ ! -e dup4.zip ] && [ ! -e dup5.zip ] || fail "made an archive with dups"

# The archive never includes itself
(cd tree && "$ZIP" -qr self.zip . && names self.zip >../got && rm self.zip)
grep -q self got && fail "archive includes itself"
grep -qx a.txt got || fail "recursing . names: $(cat got)"

# Junk paths, no directory entries, include and exclude patterns
"$ZIP" -qrj j.zip tree/sub
names j.zip >got
printf 'text\nrandom\n' >want
cmp -s got want || fail "-j: $(cat got)"
"$ZIP" -x 'tree/sub/deeper/*' -qrj j2.zip tree  # paths seen before -j
names j2.zip >got
printf 'Z.txt\na.txt\nb.txt\nempty\none\nrandom\n' >want
cmp -s got want || fail "-j -x: $(cat got)"
"$ZIP" -qj j3.zip tree/a.txt tree/sub/random -x a.txt -i 'tree/*'
names j3.zip >got
printf 'a.txt\nrandom\n' >want
cmp -s got want || fail "-j -x -i: $(cat got)"
"$ZIP" -qrD dd.zip tree
names dd.zip | grep -q '/$' && fail "-D kept a directory"
"$ZIP" -qr x.zip tree -x '*.txt' 'tree/sub/*'
names x.zip >got
printf 'tree/\ntree/.hidden/\ntree/empty\ntree/one\n' >want
cmp -s got want || fail "-x: $(cat got)"
"$ZIP" -qr -i '*.txt' @ i.zip tree
names i.zip >got
printf 'tree/Z.txt\ntree/a.txt\ntree/b.txt\n' >want
cmp -s got want || fail "-i list ended by @: $(cat got)"
printf '*.txt\ntree/s*\n' >patterns
"$ZIP" -qr x2.zip tree -x@patterns
names x2.zip >got
printf 'tree/\ntree/.hidden/\ntree/empty\ntree/one\n' >want
cmp -s got want || fail "-x@file: $(cat got)"
"$ZIP" -qr x3.zip tree -x 'tree/[ab].txt'
names x3.zip | grep -q 'tree/a.txt' && fail "-x [set]"
names x3.zip | grep -q 'tree/Z.txt' || fail "-x [set] too broad"
"$ZIP" -qr -nw x4.zip tree -x 'tree/*.txt'
names x4.zip | grep -q 'tree/a.txt' || fail "-nw still matched wildcards"
"$ZIP" -qr -nw x5.zip tree -x 'tree/?.txt'  # as in Info-ZIP, ? still is
names x5.zip | grep -q 'tree/a.txt' && fail "-nw did not match ?"
names x5.zip | grep -q 'tree/one' || fail "-nw ? too broad"
"$ZIP" -qr x8.zip tree/sub -x 'tree/sub/**'  # needs a byte, as in Info-ZIP
[ "$(names x8.zip)" = tree/sub/ ] || fail "trailing **: $(names x8.zip)"

# Patterns are normalized as names are: ./ and leading / match
(cd tree && "$ZIP" -qr ../xn.zip . -x './sub/*' -i './*.txt')
names xn.zip >got
printf 'Z.txt\na.txt\nb.txt\n' >want
cmp -s got want || fail "./ patterns: $(cat got)"
"$ZIP" -qr xa.zip "$tmp/tree/sub" -x "$tmp/tree/sub/deeper/*"
names xa.zip >got
printf '%s/tree/sub/\n%s/tree/sub/random\n' "${tmp#/}" "${tmp#/}" >want
cmp -s got want || fail "absolute pattern: $(cat got)"

# @file within a list, with CR line endings, and a missing pattern file
printf '*.txt\rtree/s*\r' >patterns.cr
"$ZIP" -qr x6.zip tree -x nomatch @patterns.cr
names x6.zip >got
printf 'tree/\ntree/.hidden/\ntree/empty\ntree/one\n' >want
cmp -s got want || fail "-x @file: $(cat got)"
expect_status 18 "$ZIP" -qr x7.zip tree -x @missing.lst
expect_status 16 "$ZIP" -qr x7.zip tree -x@

# Names from standard input, before any arguments, as Info-ZIP reads
# them: a line ends at any CR or LF, and a name at a NUL
printf 'tree/a.txt\r\ntree/one\n\ntree/sub\n' | "$ZIP" -q at.zip -@
names at.zip >got
printf 'tree/a.txt\ntree/one\ntree/sub/\n' >want
cmp -s got want || fail "-@: $(cat got)"
printf 'tree/one\rtree/b.txt\r\r\n' | "$ZIP" -q at1.zip tree/a.txt -@
names at1.zip >got
printf 'tree/one\ntree/b.txt\ntree/a.txt\n' >want
cmp -s got want || fail "-@ order and CR: $(cat got)"
printf 'tree/a.txt\0tree/one\0' | "$ZIP" -qX at2.zip -@
printf 'tree/a.txt\n' | "$ZIP" -qX at3.zip -@
cmp -s at2.zip at3.zip || fail "-@ name not cut at NUL: $(names at2.zip)"

# Symbolic links: followed by default, stored as links with -y
if ln -s a.txt tree/link 2>/dev/null; then
    "$ZIP" -q l1.zip tree/link
    zipinfo l1.zip | grep -q '^-' || fail "link not followed"
    "$ZIP" -qy l2.zip tree/link
    zipinfo l2.zip | grep -q '^l' || fail "-y did not store a link"
    [ "$(unzip -p l2.zip tree/link)" = a.txt ] || fail "-y link target"
    verify l2.zip
    ln -s . tree/sub/loop
    "$ZIP" -qr loop.zip tree/sub
    verify loop.zip
    "$ZIP" -qry loop2.zip tree/sub
    verify loop2.zip
    rm tree/link tree/sub/loop
fi

# UTF-8 names get bit 11; ASCII names do not
u=$(printf 'caf\303\251.txt')
if printf u >"$u" 2>/dev/null && [ -n "$PY" ]; then
    "$ZIP" -q u.zip "$u" tree/a.txt
    verify u.zip
    grep -q "^$u 8 0x800 " check.out || grep -q "^$u 0 0x800 " check.out ||
        fail "UTF-8 flag: $(cat check.out)"
    grep -q '^tree/a.txt [08] 0x0 ' check.out || fail "ASCII flag: $(cat check.out)"
fi

# Merging into an existing archive: replaced in place, new appended
"$ZIP" -q m.zip tree/a.txt tree/one
"$ZIP" m.zip tree/b.txt tree/a.txt >out
progress out >got
printf 'updating: tree/a.txt\n  adding: tree/b.txt\n' >want
cmp -s got want || fail "merge messages: $(cat out)"
names m.zip >got
printf 'tree/a.txt\ntree/one\ntree/b.txt\n' >want
cmp -s got want || fail "merge order: $(cat got)"
verify m.zip

# Update and freshen compare times
touch -t 202001010000 tree/a.txt tree/one tree/b.txt
rm -f u.zip
"$ZIP" -q u.zip tree/a.txt tree/one
cp u.zip u0.zip
"$ZIP" -u u.zip tree/a.txt tree/one >out
[ ! -s out ] || fail "-u with nothing newer: $(cat out)"
cmp -s u.zip u0.zip || fail "-u with nothing newer changed the archive"
touch -t 202101010000 tree/one
"$ZIP" -u u.zip tree/a.txt tree/one tree/b.txt >out
progress out >got
printf 'updating: tree/one\n  adding: tree/b.txt\n' >want
cmp -s got want || fail "-u: $(cat out)"
touch -t 202201010000 tree/a.txt
"$ZIP" -f u.zip tree/a.txt tree/empty >out
progress out >got
printf 'freshening: tree/a.txt\n' >want
cmp -s got want || fail "-f: $(cat out)"
names u.zip | grep -q empty && fail "-f added a file"
verify u.zip
expect_status 12 "$ZIP" -f missing.zip tree/a.txt

# As in Info-ZIP, -u and -f compare the Unix time of an entry's UT field
# when it has one, so that the time zone does not matter, else DOS times
printf one >tz.txt
TZ=UTC0 touch -t 202001011000.00 tz.txt
TZ=JST-9 "$ZIP" -q tz.zip tz.txt
TZ=UTC0 touch -t 202001011100.00 tz.txt
TZ=UTC0 "$ZIP" -u tz.zip tz.txt >out
grep -q '^updating: tz.txt' out || fail "-u east to west: $(cat out)"
cp tz.zip tz0.zip
TZ=JST-9 "$ZIP" -u tz.zip tz.txt >out
[ ! -s out ] && cmp -s tz.zip tz0.zip || fail "-u west to east: $(cat out)"
TZ=UTC0 touch -t 202001011000.01 tz.txt
"$ZIP" -q tz1.zip tz.txt
"$ZIP" -qX tz2.zip tz.txt
TZ=UTC0 touch -t 202001011000.02 tz.txt  # the same DOS time, rounded up
"$ZIP" -u tz1.zip tz.txt | grep -q updating || fail "-u by UT time"
"$ZIP" -u tz2.zip tz.txt | grep -q updating && fail "-u -X by DOS time"

# Under SOURCE_DATE_EPOCH, files changed since the epoch are found newer
# than entries clamped to it (and unchanged ones rewritten identically)
printf 'version 1.0.0' >ver.txt
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -q ver1.zip ver.txt
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX ver2.zip ver.txt
cp ver1.zip ver0.zip
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qu ver1.zip ver.txt
cmp -s ver1.zip ver0.zip || fail "-u under SOURCE_DATE_EPOCH changed bytes"
printf 'version 1.0.1' >ver.txt
for v in ver1.zip ver2.zip; do
    for mode in -u -f -FS; do
        cp $v ver.zip
        SOURCE_DATE_EPOCH=1700000000 "$ZIP" -q $mode ver.zip ver.txt
        [ "$(unzip -p ver.zip ver.txt)" = 'version 1.0.1' ] ||
            fail "$mode $v under SOURCE_DATE_EPOCH"
    done
done

# Filesync: changed entries updated, missing ones deleted
cp -R tree fs
"$ZIP" -qr fs.zip fs
rm fs/b.txt
printf new >fs/new.txt
touch -t 203001010000 fs/one
"$ZIP" -FS -r fs.zip fs >out
grep -qx 'deleting: fs/b.txt' out || fail "-FS delete: $(cat out)"
grep -q '^updating: fs/one ' out || fail "-FS update: $(cat out)"
grep -q '^  adding: fs/new.txt ' out || fail "-FS add: $(cat out)"
grep -q 'fs/a.txt' out && fail "-FS updated an unchanged file: $(cat out)"
verify fs.zip
extract_same fs.zip fs
"$ZIP" -FS -r fs.zip fs >out
[ "$(cat out)" = "Archive is current" ] || fail "-FS current: $(cat out)"
"$ZIP" -qFS -r fs.zip fs >out
[ ! -s out ] || fail "-qFS current: $(cat out)"

# Filesync that finds nothing has nothing to do, rather than deleting
# every entry
cp fs.zip fs0.zip
expect_status 12 "$ZIP" -FS fs.zip
expect_status 12 "$ZIP" -FS fs.zip missing
expect_status 12 "$ZIP" -FS -r fs.zip fs -x '*'
expect_status 12 "$ZIP" -FS -@ fs.zip </dev/null
cmp -s fs.zip fs0.zip || fail "-FS with nothing found changed the archive"

# As in Info-ZIP, a path not on disk selects the entries it matches as
# a pattern, and -u and -f without paths select every entry: the file
# each names is examined (without recursion, -D, or -j), and if it is
# gone the entry is kept, or deleted under -FS
mkdir -p sel/d1/s sel/d2
printf a >sel/d1/a.txt
printf b >sel/d1/b.log
printf c >sel/d1/s/c.txt
printf g >sel/d1/gone
printf d >sel/d2/d.txt
(cd sel && touch -t 202001010000 d1/* d1/s/c.txt d2/d.txt d2 d1 &&
 "$ZIP" -qr ../sel.zip d1 d2 && rm d1/gone &&
 touch -t 202301010000 d1/a.txt d1/s/c.txt && touch -t 202001010000 d1)
selz() {  # expected-progress zip-arguments...
    want=$1
    shift
    cp sel.zip sel/t.zip
    (cd sel && "$ZIP" "$@" >../out 2>../err) || true
    progress out | tr '\n' ' ' >got
    [ "$(cat got)" = "$want" ] || fail "zip $*: $(cat got) $(cat err)"
}
selz 'updating: d1/a.txt updating: d1/s/c.txt ' -u t.zip d2/d.txt 'd1/*'
selz 'updating: d1/a.txt updating: d1/s/c.txt ' -u -@ t.zip <<EOF
d1/*.txt
EOF
selz 'updating: d1/a.txt updating: d1/s/c.txt ' -u t.zip
selz 'freshening: d1/a.txt freshening: d1/s/c.txt ' -f t.zip
selz 'freshening: d1/a.txt freshening: d1/s/c.txt ' -f t.zip '*'
selz 'updating: d1/a.txt ' -u t.zip -x '*s/*'
selz 'updating: d1/a.txt ' -u -nw -D -j t.zip 'd1/a?txt'
selz 'updating: d1/a.txt updating: d1/s/c.txt ' -u t.zip 'd1/*' ./d1/a.txt
selz 'updating: d1/ updating: d1/a.txt updating: d1/b.log updating: d1/s/ updating: d1/s/c.txt ' t.zip 'd1/*'
names sel/t.zip | grep -q d1/gone || fail "a missing file's entry was lost"
selz 'deleting: d1/ updating: d1/a.txt deleting: d1/b.log deleting: d1/gone deleting: d1/s/ updating: d1/s/c.txt ' -FS t.zip d2 d2/d.txt 'd1/*.txt'
cp sel.zip sel/t.zip
(cd sel && expect_status 12 "$ZIP" -u t.zip 'd1/g*')
(cd sel && expect_status 12 "$ZIP" -u -nw t.zip 'd1/*')
(cd sel && "$ZIP" -u t.zip 'nomatch*' 2>&1 | grep -q 'not matched: nomatch') ||
    fail "pattern matching no entry"
(cd sel && "$ZIP" -u t.zip 'd1/g*' 2>&1 | grep -q 'not matched') &&
    fail "pattern matching an entry of a missing file"

# An entry selected by name that has changed between file and directory
# is kept, with Info-ZIP's warning
rm sel/d1/b.log
mkdir sel/d1/b.log
touch -t 202001010000 sel/d1
selz 'updating: d1/a.txt updating: d1/s/c.txt ' -u t.zip
grep -q 'file and directory with the same name: d1/b.log' err ||
    fail "file then directory: $(cat err)"
[ "$(unzip -p sel/t.zip d1/b.log)" = b ] || fail "file then directory kept"
(cd sel && expect_status 18 "$ZIP" -u t.zip)

# Entry names zip would not make are not read as paths, so that an
# untrusted archive cannot select any file
if [ -n "$PY" ]; then
    $PY -c 'import sys, zipfile as z
a = z.ZipFile(sys.argv[1], "w"); a.writestr(z.ZipInfo(sys.argv[2]), "x")' \
        abs.zip "$tmp/tree/a.txt"
    expect_status 12 "$ZIP" -f abs.zip
    [ "$(unzip -p abs.zip)" = x ] || fail "absolute entry name freshened"
fi

# Delete
"$ZIP" -d m.zip 'tree/o*' nomatch >out 2>&1
grep -q 'deleting: tree/one' out || fail "-d: $(cat out)"
grep -q 'name not matched: nomatch' out || fail "-d warning: $(cat out)"
names m.zip | grep -q one && fail "-d left the entry"
verify m.zip
expect_status 12 "$ZIP" -d m.zip nomatch
expect_status 12 "$ZIP" -d missing.zip tree/a.txt
"$ZIP" -qd m.zip '*'
[ "$(wc -c <m.zip | tr -d ' ')" = 22 ] || fail "emptied archive not 22 bytes"

# Deletion honors -x and -i, and a directory on disk names its entry
"$ZIP" -qr dx.zip tree
"$ZIP" -qd dx.zip '*' -x '*.txt'
names dx.zip >got
printf 'tree/Z.txt\ntree/a.txt\ntree/b.txt\n' >want
cmp -s got want || fail "-d -x: $(cat got)"
"$ZIP" -qd dx.zip '*' -i 'tree/a*'
names dx.zip >got
printf 'tree/Z.txt\ntree/b.txt\n' >want
cmp -s got want || fail "-d -i: $(cat got)"
cp dx.zip dx0.zip
expect_status 12 "$ZIP" -d dx.zip '*' -x '*'
cmp -s dx.zip dx0.zip || fail "-d of only excluded entries changed the archive"
"$ZIP" -qr dir.zip tree/sub
"$ZIP" -qd dir.zip tree/sub 2>out || fail "-d of a directory: $(cat out)"
names dir.zip >got
printf 'tree/sub/deeper/\ntree/sub/deeper/text\ntree/sub/random\n' >want
cmp -s got want || fail "-d of a directory: $(cat got)"

# -d names are normalized like names, and those on disk are literal,
# wildcards and all, as in Info-ZIP
"$ZIP" -q dn.zip tree/a.txt tree/b.txt tree/one
"$ZIP" -qd dn.zip ./tree/a.txt '/tree/b.*'
[ "$(names dn.zip)" = tree/one ] || fail "-d ./ and /: $(names dn.zip)"
mkdir -p 'lit/s*' lit/sx
printf q >'lit/s*/q'
printf r >lit/sx/r
"$ZIP" -qr lit.zip lit
"$ZIP" -qd lit.zip 'lit/s*'
names lit.zip >got
printf 'lit/\nlit/s*/q\nlit/sx/\nlit/sx/r\n' >want
cmp -s got want || fail "-d of a name on disk: $(cat got)"
expect_status 12 "$ZIP" -qdD lit.zip lit

# A -d name that is a special file on disk marks nothing
mkdir ffd
printf x >ffd/fifo
(cd ffd && "$ZIP" -q ../ff.zip fifo)
if mkfifo fifo 2>/dev/null; then
    "$ZIP" -d ff.zip fifo >out 2>&1 && fail "-d of a FIFO name succeeded"
    grep -q 'special file: fifo' out || fail "-d of a FIFO: $(cat out)"
    [ "$(names ff.zip)" = fifo ] || fail "-d of a FIFO deleted the entry"
    rm fifo
fi

# Copied entries keep their bytes; -X strips their extra fields
"$ZIP" -qr k1.zip tree
"$ZIP" -q k1.zip tree/a.txt
verify k1.zip
extract_same k1.zip tree
"$ZIP" -qX k1.zip tree/b.txt
verify k1.zip

# Errors and warnings
expect_status 12 "$ZIP" nothing.zip missing
[ ! -e nothing.zip ] || fail "created an archive with nothing to do"
expect_status 0 "$ZIP" some.zip missing tree/a.txt
expect_status 16 "$ZIP" dup.zip tree/a.txt ./tree/a.txt
expect_status 16 "$ZIP" -K bad.zip tree/a.txt
expect_status 16 "$ZIP" -e bad.zip tree/a.txt
expect_status 16 "$ZIP" --bogus bad.zip tree/a.txt
expect_status 16 "$ZIP" - tree/a.txt
expect_status 16 "$ZIP" -u -d bad.zip tree/a.txt
echo junk >junk.zip
expect_status 3 "$ZIP" junk.zip tree/a.txt
head -c 100 t.zip >trunc.zip
expect_status 3 "$ZIP" trunc.zip tree/a.txt

# Whatever is at the archive path must be a zip file, as Info-ZIP finds
# before any work: not an empty file (not even to add to itself), a
# directory, a FIFO, or a device
: >empty.zip
for mode in -q -qu -qf -qd -qFS; do
    expect_status 3 "$ZIP" $mode empty.zip tree/a.txt
done
[ ! -s empty.zip ] || fail "an empty archive was written"
(cd tree && : >self.zip && expect_status 3 "$ZIP" -r self.zip . && rm self.zip)
mkdir isdir.zip
"$ZIP" -r isdir.zip tree >out 2>&1 && fail "directory archive succeeded"
grep -q 'adding:' out && fail "directory archive did work first: $(cat out)"
grep -q 'structure invalid' out || fail "directory archive: $(cat out)"
if mkfifo fifo.zip 2>/dev/null; then
    expect_status 3 "$ZIP" fifo.zip tree/a.txt
    [ -p fifo.zip ] || fail "FIFO archive replaced"
fi
ln -s /dev/null null.zip && expect_status 3 "$ZIP" null.zip tree/a.txt

if [ "$(id -u)" != 0 ]; then
    cp tree/a.txt unreadable
    chmod 000 unreadable
    expect_status 18 "$ZIP" r.zip tree/b.txt unreadable
    names r.zip >got
    printf 'tree/b.txt\n' >want
    cmp -s got want || fail "unreadable file: $(cat got)"
    chmod 644 unreadable

    # A directory after a failed file gets none of its fields
    mkdir -p fd/sub
    printf x >fd/a
    chmod 000 fd/a
    expect_status 18 "$ZIP" -r fd.zip fd
    verify fd.zip
    names fd.zip >got
    printf 'fd/\nfd/sub/\n' >want
    cmp -s got want || fail "failed file then directory: $(cat got)"
    chmod 644 fd/a

    # An entry whose replacement cannot be read is kept, in every mode
    for mode in -q -qu -qf -qFS; do
        rm -f s.zip
        printf old >stale
        "$ZIP" -q s.zip stale tree/b.txt
        chmod 000 stale
        touch -t 203001010000 stale
        expect_status 18 "$ZIP" $mode s.zip stale tree/b.txt
        verify s.zip
        [ "$(unzip -p s.zip stale)" = old ] || fail "$mode dropped the entry"
        chmod 644 stale
    done
    chmod 000 stale
    "$ZIP" s.zip stale >out 2>&1 && fail "unreadable replacement succeeded"
    grep -qx 'zip warning: will just copy entry over: stale' out ||
        fail "copy over warning: $(cat out)"
    chmod 644 stale
fi
expect_status 0 "$ZIP" -h

# Without arguments, or without an archive name, Info-ZIP streams to
# standard output, which is rejected, unless it is a terminal: then
# there is usage, or for an archive an error
expect_status 16 "$ZIP"
expect_status 16 "$ZIP" -qr
if script -q /dev/null "$ZIP" </dev/null >out 2>&1 ||  # BSD
   script -qec "$ZIP" /dev/null </dev/null >out 2>&1; then  # util-linux
    grep -q usage out || fail "no usage on a terminal: $(cat out)"
fi
expect_status 16 "$ZIP" -- dash.zip tree/a.txt  # before the archive name
"$ZIP" -q dash.zip -- -x tree/a.txt 2>/dev/null
[ "$(names dash.zip)" = tree/a.txt ] || fail "-- then -x: $(names dash.zip)"

# Version and license, even with other arguments; -v only alone
"$ZIP" -v </dev/null >out
grep -q '^tugz zip [0-9]' out || fail "-v: $(cat out)"
"$ZIP" --version info.zip tree/a.txt >out
grep -q '^tugz zip [0-9]' out || fail "--version: $(cat out)"
"$ZIP" -L info.zip tree/a.txt >out
grep -q 'public domain' out || fail "-L: $(cat out)"
[ ! -e info.zip ] || fail "--version or -L made an archive"

# Options from ZIPOPT or, if it has none, ZIP come first; ZIPOPT splits
# at whitespace except within double quotes
env ZIPOPT=-q "$ZIP" env1.zip tree/a.txt >out
[ ! -s out ] || fail "ZIPOPT=-q: $(cat out)"
env ZIPOPT=' ' ZIP='	-q ' "$ZIP" env2.zip tree/a.txt >out
[ ! -s out ] || fail "ZIP=-q: $(cat out)"
env ZIPOPT=-r ZIP=-q "$ZIP" env3.zip tree/sub >out
grep -q 'adding: tree/sub/random' out || fail "ZIPOPT=-r ZIP=-q: $(cat out)"
printf s >'tree/a b'
env ZIPOPT='-q -x "tree/a b" tree/b.txt' "$ZIP" -r env4.zip tree
names env4.zip | grep -q 'tree/a b' && fail "ZIPOPT quoted pattern"
names env4.zip | grep -q tree/b.txt && fail "ZIPOPT pattern list"
rm 'tree/a b'
expect_status 16 env ZIPOPT=-e "$ZIP" env5.zip tree/a.txt
env ZIPOPT=-v "$ZIP" </dev/null >out
grep -q '^tugz zip [0-9]' out || fail "ZIPOPT=-v: $(cat out)"

# Options -d ignores, as Info-ZIP warns
"$ZIP" -q dw.zip tree/a.txt tree/b.txt
"$ZIP" -d -r dw.zip tree/a.txt >out 2>&1
grep -q 'invalid option(s) used with -d; ignored' out ||
    fail "-d -r: $(cat out)"
[ "$(names dw.zip)" = tree/b.txt ] || fail "-d -r: $(names dw.zip)"

# A new archive gets the permissions of a new file, while a replaced
# one keeps its own
(umask 077 && "$ZIP" -q perm.zip tree/a.txt)
[ "$(ls -l perm.zip | cut -c1-10)" = "-rw-------" ] || fail "new archive mode"
chmod 640 perm.zip
(umask 022 && "$ZIP" -q perm.zip tree/b.txt)
[ "$(ls -l perm.zip | cut -c1-10)" = "-rw-r-----" ] || fail "replaced mode"

# With standard output closed, progress lines stay out of the archive
"$ZIP" -r closed.zip tree >&-
verify closed.zip

# A reader that goes away (SIGPIPE) leaves no temporary file behind
mkdir pipe
(cd pipe && "$ZIP" -r out.zip ../tree | true)
case "$(ls pipe)" in zi*) fail "temporary file left on SIGPIPE";; esac

if [ -n "$SLOW" ]; then
    # Zip64: a 5 GiB file, compressed and stored (pushing a following
    # entry's offset past 4 GiB), then merged into
    dd if=/dev/zero of=big bs=1 count=0 seek=5368709120 2>/dev/null
    "$ZIP" -q1 big1.zip big tree/a.txt
    verify big1.zip
    [ "$(unzip -p big1.zip big | wc -c | tr -d ' ')" = 5368709120 ] ||
        fail "5 GiB round trip"
    "$ZIP" -q0 big0.zip big tree/a.txt
    verify big0.zip
    "$ZIP" -q big0.zip tree/b.txt
    verify big0.zip
    [ "$(unzip -p big0.zip tree/a.txt)" = "hello hello hello hello" ] ||
        fail "entry past 4 GiB"
    rm big big1.zip big0.zip

    # Zip64: more than 65,535 entries
    mkdir many
    (cd many && awk 'BEGIN{for(i=0;i<70000;i++)print i}' | xargs touch)
    "$ZIP" -qr many.zip many
    verify many.zip
    [ "$(names many.zip | wc -l | tr -d ' ')" = 70001 ] || fail "70,000 entries"
    "$ZIP" -q many.zip tree/a.txt
    verify many.zip
    [ "$(names many.zip | wc -l | tr -d ' ')" = 70002 ] || fail "merge many"
fi

echo "zip tests pass"
