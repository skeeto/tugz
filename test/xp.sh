#!/bin/sh
# Windows end-to-end tests of gzip.exe, zip.exe, and unzip.exe that need
# nothing newer than Windows XP: w64devkit's shell and C compiler, which
# builds test/winhelp.c for the rest, and no PowerShell, tar, or pseudo
# console, on which zip_windows.sh and unzip_windows.sh rely. They cover
# what the platform layer does, through its XP fallbacks there: times
# and attributes kept, outputs replaced or discarded, recursion and
# hidden files, Unicode names and paths longer than MAX_PATH, unzip's
# prompt read from a pipe, and Ctrl+C and Ctrl+Break. They pass on later
# Windows too, where the other scripts test more.
# Usage: sh test/xp.sh ./gzip.exe ./zip.exe ./unzip.exe
# CC names the compiler (default cc).
set -e

if [ $# != 3 ]; then
    echo "usage: sh test/xp.sh GZIP.EXE ZIP.EXE UNZIP.EXE" >&2
    exit 1
fi
unset GZIP ZIP ZIPOPT UNZIP UNZIPOPT  # options for the programs
abs() {
    echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
}
GZIP=$(abs "$1")
ZIP=$(abs "$2")
UNZIP=$(abs "$3")
testdir=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
WH=$tmp/winhelp.exe
# (the helper's rmtree for paths too long for rm, which then removes the
# helper itself)
trap 'cd / && { "$WH" rmtree "$tmp" 2>/dev/null || true
                rm -rf "$tmp"; }' EXIT
cd "$tmp"

# A failure is marked, so that one within a pipeline or $(...), whose
# status no one sees, still fails the run at its end
fail() {
    echo "FAIL: $*" >&2
    : >"$tmp/FAILED"
    exit 1
}

expect_status() {
    want=$1
    shift
    set +e
    "$@" >/dev/null 2>&1 </dev/null
    got=$?
    set -e
    [ "$got" = "$want" ] || fail "expected status $want, got $got: $*"
}

[ "$(uname -s)" = Windows_NT ] || fail "xp.sh tests Windows programs"
${CC:-cc} -o "$WH" "$testdir/winhelp.c" || fail "cannot build winhelp.c"

attrs() {
    "$WH" attrs "$1" | tr -d '\r'
}

# What is in the current directory, on one line
here() {
    ls -A | tr '\n' ' '
}

# gzip: times kept both ways, -f replacing, and a truncated input leaving
# nothing but itself
mkdir g
cd g
head -c 200000 /dev/urandom >data
awk 'BEGIN{for(i=0;i<20000;i++)print i, i*i}' >>data
cp data orig
touch -t 202001021234.56 data
t=$(stat -c %Y data)
"$GZIP" data || fail "gzip: status $?"
[ ! -e data ] && [ "$(stat -c %Y data.gz)" = "$t" ] ||
    fail "gzip: time $(stat -c %Y data.gz), not $t"
"$GZIP" -d data.gz || fail "gzip -d: status $?"
cmp -s data orig || fail "gzip -d: contents"
[ "$(stat -c %Y data)" = "$t" ] || fail "gzip -d: time $(stat -c %Y data)"
[ "$(here)" = "data orig " ] || fail "gzip -d: left $(here)"

"$GZIP" -k data
cp data.gz before.gz
expect_status 2 "$GZIP" -k data
cmp -s data.gz before.gz || fail "gzip: replaced an output without -f"
printf old >data.gz
"$GZIP" -kf data || fail "gzip -f: status $?"
"$GZIP" -dc data.gz | cmp -s - data || fail "gzip -f: contents"
printf old >data
"$GZIP" -df data.gz || fail "gzip -df: status $?"
cmp -s data orig || fail "gzip -df: contents"
rm before.gz

"$GZIP" -c data >whole.gz
head -c $(($(wc -c <whole.gz) / 2)) whole.gz >trunc.gz
rm whole.gz
expect_status 1 "$GZIP" -d trunc.gz
[ "$(here)" = "data orig trunc.gz " ] || fail "truncated input: $(here)"
rm trunc.gz

# ...and read-only and hidden attributes, with the archive bit set
cp orig ro
"$WH" attrib +r +h -a ro
"$GZIP" ro || fail "gzip, read-only hidden: status $?"
[ "$(attrs ro.gz)" = "ReadOnly, Hidden, Archive" ] ||
    fail "gzip, read-only hidden: $(attrs ro.gz)"
"$GZIP" -d ro.gz || fail "gzip -d, read-only hidden: status $?"
[ "$(attrs ro)" = "ReadOnly, Hidden, Archive" ] ||
    fail "gzip -d, read-only hidden: $(attrs ro)"
cmp -s ro orig || fail "gzip -d, read-only hidden: contents"
"$WH" attrib -r -h ro
cd ..

# zip: -r, leaving out hidden files unless -S, then -ur and -d, none
# leaving a temporary file (zi*)
mkdir -p z/tree/sub
cd z
printf 'hello\n' >tree/a.txt
cp ../g/orig tree/sub/data
: >tree/empty
printf ro >tree/ro.txt
printf hid >tree/hid.txt
touch -t 202001021234.56 tree/a.txt tree/sub/data
"$WH" attrib +r tree/ro.txt
"$WH" attrib +h tree/hid.txt
"$ZIP" -qr a.zip tree || fail "zip -r: status $?"
"$ZIP" -qrS s.zip tree || fail "zip -rS: status $?"
"$UNZIP" -l a.zip >a.list
"$UNZIP" -l s.zip >s.list
for f in a.txt sub/data empty ro.txt; do
    grep -q " tree/$f\$" a.list || fail "zip -r: no $f"
done
grep -q hid.txt a.list && fail "zip -r: added a hidden file"
grep -q ' tree/hid.txt$' s.list || fail "zip -rS: no hidden file"

printf 'changed\n' >tree/a.txt
touch -t 202101021234.56 tree/a.txt
printf 'new\n' >tree/new.txt
"$ZIP" -ur a.zip tree >ur.out || fail "zip -ur: status $?"
# (tree/ itself, its time changed, may be updated too)
[ "$(grep -cv '^updating: tree/ ' ur.out)" = 2 ] &&
    grep -q '^updating: tree/a.txt ' ur.out &&
    grep -q '^  adding: tree/new.txt ' ur.out || fail "zip -ur: $(cat ur.out)"
[ "$("$UNZIP" -p a.zip tree/a.txt)" = changed ] || fail "zip -ur: contents"
"$ZIP" -ur a.zip tree >ur.out || fail "zip -ur, nothing newer: status $?"
[ ! -s ur.out ] || fail "zip -ur, nothing newer: $(cat ur.out)"
"$ZIP" -q -d a.zip tree/new.txt || fail "zip -d: status $?"
"$UNZIP" -l a.zip | grep -q new.txt && fail "zip -d: entry kept"
"$UNZIP" -tq a.zip >/dev/null || fail "unzip -t of zip's archive"
[ "$(ls | grep '^zi' || true)" = "" ] || fail "zip: left $(here)"

# unzip: contents, times, and attributes (read-only, hidden) restored,
# -t, -p, -n, -o (over a read-only file), -d, and the prompt to replace,
# answered through a pipe
mkdir u
cd u
"$UNZIP" -q ../a.zip || fail "unzip: status $?"
"$UNZIP" -q ../s.zip tree/hid.txt || fail "unzip of a hidden file: status $?"
cmp -s tree/sub/data ../tree/sub/data || fail "unzip: contents"
for f in a.txt sub/data; do
    [ "$(stat -c %Y tree/$f)" = "$(stat -c %Y ../tree/$f)" ] ||
        fail "unzip: $f time $(stat -c %Y tree/$f)"
done
case "$(attrs tree/ro.txt)" in
*ReadOnly*) ;;
*) fail "unzip: tree/ro.txt $(attrs tree/ro.txt)" ;;
esac
case "$(attrs tree/hid.txt)" in
*Hidden*) ;;
*) fail "unzip: tree/hid.txt $(attrs tree/hid.txt)" ;;
esac
"$UNZIP" -t ../a.zip >t.out || fail "unzip -t: status $?"
grep -q 'No errors detected' t.out || fail "unzip -t: $(cat t.out)"
"$UNZIP" -p ../a.zip tree/sub/data | cmp -s - ../tree/sub/data ||
    fail "unzip -p"
printf 'mine\n' >tree/a.txt
"$UNZIP" -qn ../a.zip || fail "unzip -n: status $?"
[ "$(cat tree/a.txt)" = mine ] || fail "unzip -n replaced a file"
printf 'n\n' | "$UNZIP" ../a.zip tree/a.txt >p.out 2>&1 ||
    fail "unzip, prompt answered n: status $?"
[ "$(cat tree/a.txt)" = mine ] || fail "unzip, prompt answered n: replaced"
grep -q 'replace tree/a.txt? \[y\]es, \[n\]o' p.out ||
    fail "unzip prompt: $(cat p.out)"
printf 'y\n' | "$UNZIP" ../a.zip tree/a.txt >p.out 2>&1 ||
    fail "unzip, prompt answered y: status $?"
[ "$(cat tree/a.txt)" = changed ] || fail "unzip, prompt answered y: kept"
printf 'mine\n' >tree/a.txt
"$UNZIP" -qo ../a.zip || fail "unzip -o: status $?"
[ "$(cat tree/a.txt)" = changed ] && [ "$(cat tree/ro.txt)" = ro ] ||
    fail "unzip -o: contents"
"$UNZIP" -q ../a.zip -d new || fail "unzip -d: status $?"
cmp -s new/tree/sub/data ../tree/sub/data || fail "unzip -d: contents"
"$WH" attrib -r tree/ro.txt
"$WH" attrib -r new/tree/ro.txt
cd ../..

# Unicode names, beyond any code page, and a path longer than MAX_PATH,
# made and read by the helper (\uXXXX escapes, \\?\ paths), through zip
# and unzip and back
mkdir n
cd n
names='\u00e9t\u00e9.txt \u0436\u0443\u043a.txt \u65e5\u672c.txt'
names="$names \ud83d\ude00.txt"  # (beyond the BMP)
for name in $names; do
    "$WH" write "src/$name" "$name" || fail "cannot make src/$name"
done
seg=abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz
long=$seg/$seg/$seg/$seg/$seg/long.txt
"$WH" write "src/$long" long || fail "cannot make a long path"
"$ZIP" -qr u.zip src || fail "zip, long and Unicode names: status $?"
"$UNZIP" -tq u.zip >/dev/null || fail "unzip -t, long and Unicode names"
"$UNZIP" -q u.zip -d out || fail "unzip, long and Unicode names: status $?"
for name in $names; do
    [ "$("$WH" cat "out/src/$name")" = "$name" ] || fail "no out/src/$name"
done
[ "$("$WH" cat "out/src/$long")" = long ] || fail "no long path"
[ "$("$GZIP" -c "src/$long" | "$GZIP" -dc)" = long ] ||
    fail "gzip -c of a long path"
cd ..

# Ctrl+C and Ctrl+Break, sent by the helper once a program has written a
# megabyte, in the midst of a large file, and from a console of the
# helper's own, so that no other process sees the event. zip and unzip
# write the text they hold (standard output here a file) and end with
# the status of an interrupted console program; zip leaves no archive
# or temporary file, unzip none of the file it was extracting, and
# gzip, which leaves the event to the system, discards its output.
mkdir c
cd c
mkdir in
i=0
while [ $i -lt 20 ]; do
    echo $i >in/f$i
    i=$((i + 1))
done
"$ZIP" -r z.zip in >want.out 2>&1 || fail "zip: status $?"
rm z.zip
head -c 48000000 /dev/urandom >in/zz_big
for e in c b; do
    st=$("$WH" ctrl $e 'zi*' 1000000 out.txt err.txt "$ZIP" -r z.zip in |
         tr -d '\r')
    [ "$st" = C000013A ] || fail "zip, Ctrl+$e: $st $(cat err.txt)"
    [ ! -e z.zip ] && [ "$(ls | grep '^zi' || true)" = "" ] ||
        fail "zip, Ctrl+$e: left $(here)"
    cmp -s out.txt want.out || fail "zip, Ctrl+$e: wrote $(cat out.txt)"
done

sum=$(md5sum <in/zz_big)
for e in c b; do
    st=$("$WH" ctrl $e 'in/zz_big.gz' 1000000 out.txt err.txt \
         "$GZIP" in/zz_big | tr -d '\r')
    [ "$st" = C000013A ] || fail "gzip, Ctrl+$e: $st $(cat err.txt)"
    [ ! -e in/zz_big.gz ] || fail "gzip, Ctrl+$e: output kept"
    [ "$(md5sum <in/zz_big)" = "$sum" ] || fail "gzip, Ctrl+$e: input lost"
done
rm -r in

# For unzip, an archive of small files and, last, 512 MB of zeros (a
# file extended, never written, so quick to make), and its like with
# 4 KiB, whose run's text the interrupted runs' must begin
mkdir in
for i in 0 1 2 3 4 5 6 7; do
    echo $i >in/f$i
done
"$WH" size in/zeros 4096
"$ZIP" -qr zeros.zip in || fail "zip: status $?"
"$UNZIP" zeros.zip -d x >want.out 2>&1 ||
    fail "unzip: status $? $(cat want.out)"
rm -r x zeros.zip
"$WH" size in/zeros 536870912
"$ZIP" -qr zeros.zip in || fail "zip, zeros: status $?"
rm -r in
for e in c b; do
    st=$("$WH" ctrl $e 'x/in/zeros' 1000000 out.txt err.txt \
         "$UNZIP" zeros.zip -d x | tr -d '\r')
    [ "$st" = C000013A ] || fail "unzip, Ctrl+$e: $st $(cat err.txt)"
    [ ! -e x/in/zeros ] || fail "unzip, Ctrl+$e: partial file kept"
    [ "$(ls x/in | tr '\n' ' ')" = "f0 f1 f2 f3 f4 f5 f6 f7 " ] ||
        fail "unzip, Ctrl+$e: $(ls x/in)"
    grep -q 'inflating: x/in/zeros' out.txt &&
        head -c $(wc -c <out.txt) want.out | cmp -s - out.txt ||
        fail "unzip, Ctrl+$e: wrote $(cat out.txt)"
    rm -r x
done
cd ..

[ ! -e "$tmp/FAILED" ] || exit 1
echo "windows xp tests pass"
