#!/bin/sh
# End-to-end tests of a zip binary, verified with unzip, zipinfo, and
# Python's zipfile (via uv when available).
# Usage: sh test/zip.sh ./zip
# Set SLOW=1 to include Zip64 tests: 4 and 5 GiB files, 70,000 entries.
# These need about 10 GiB free in TMPDIR (a stored 5 GiB archive and the
# temporary file that merging into it writes).
set -e

unset ZIPOPT ZIP  # options for zip, and ZIP unexported for the binary
unset SOURCE_DATE_EPOCH  # set where tested, as by a reproducible build
ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
CHECK=$(cd "$(dirname "$0")" && pwd)/zipcheck.py
if command -v uv >/dev/null 2>&1; then
    PY="uv run --no-project python3"
elif command -v python3 >/dev/null 2>&1; then
    PY=python3
else
    PY=
fi
for tool in unzip zipinfo; do
    if ! command -v $tool >/dev/null 2>&1; then
        echo "zip.sh: Info-ZIP's unzip and zipinfo are required" \
             "to verify archives, but $tool was not found" >&2
        exit 1
    fi
done
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

# Run zip in the background, as process $pid in job $bg, and stop it
# while its temporary file is in a directory, so before it replaces the
# archive. Fails if zip finished first: rarely, unless it has little to
# write, as the wait spins, starting no process between seeing the file
# and stopping zip. Once $bg is waited for, zip's status is in
# bg.status, and its output in bg.out and bg.err.
bgzip() {  # directory zip-arguments...
    dir=$1
    shift
    rm -f bg.pid bg.status
    { st=0
      sh -c 'echo $$ >bg.pid && exec "$@"' sh "$ZIP" "$@" \
          >bg.out 2>bg.err || st=$?
      echo $st >bg.status; } 2>/dev/null &  # (bash reports signals)
    bg=$!
    while [ ! -e bg.status ]; do
        for f in "$dir"/zi[0-9]*; do
            if [ -e "$f" ]; then
                read pid <bg.pid
                kill -STOP $pid
                [ -e "$f" ] && return 0
                kill -CONT $pid
                return 1
            fi
        done
    done
    return 1
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
# and incompressible data, are stored at any level. Deflated entries get
# Info-ZIP's flag bits for the level, which zipinfo shows as defF (fast,
# -1 and -2), defN, and defX (maximum, -8 and -9).
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
    case $level in
    0) continue;;
    1|2) def=defF;;
    8|9) def=defX;;
    *) def=defN;;
    esac
    zipinfo l.zip | grep 'text$' | grep -q " $def " ||
        fail "text -$level, not $def: $(zipinfo l.zip | grep 'text$')"
done

# Unix modes, and the DOS read-only attribute, which Windows tools honor
mkdir -p modes/d
printf '#!/bin/sh\n' >modes/run.sh
printf r >modes/ro.txt
chmod 755 modes/run.sh
chmod 444 modes/ro.txt
chmod 750 modes/d
"$ZIP" -qr modes.zip modes
zipinfo modes.zip >out
grep -q '^drwxr-x--- .* modes/d/$' out &&
    grep -q '^-rwxr-xr-x .* modes/run.sh$' out &&
    grep -q '^-r--r--r-- .* modes/ro.txt$' out || fail "modes: $(cat out)"
zipinfo -v modes.zip >out
grep -q 'MS-DOS file attributes (01 hex): *read-only' out &&
    [ "$(grep -c 'MS-DOS file attributes (00 hex): *none' out)" = 1 ] &&
    [ "$(grep -c 'MS-DOS file attributes (10 hex): *dir' out)" = 2 ] ||
    fail "DOS attributes: $(grep 'MS-DOS file attributes' out)"

# Below -9, files with the suffixes of Info-ZIP's default -n list are
# stored without trying to compress them, as there: so without level
# flag bits, as with -0. Case matters on POSIX.
mkdir sfx
for name in a.zip b.Z c.zoo d.arc e.lzh f.arj; do
    head -c 4000 tree/sub/deeper/text >sfx/$name
done
"$ZIP" -qX0 sfx0.zip sfx/*
for level in 1 2 6 8; do
    rm -f sfx$level.zip
    "$ZIP" -qX$level sfx$level.zip sfx/*
    cmp -s sfx0.zip sfx$level.zip || fail "suffixes compressed at -$level"
done
"$ZIP" -q9 sfx9.zip sfx/*
[ "$(zipinfo sfx9.zip | grep -c defX)" = 6 ] || fail "suffixes stored at -9"
head -c 4000 tree/sub/deeper/text >sfx/g.ZIP
"$ZIP" sfx.zip sfx/a.zip sfx/g.ZIP >out
grep -q 'sfx/a.zip (stored 0%)' out || fail "suffix progress: $(cat out)"
grep -q 'sfx/g.ZIP (deflated' out || fail "suffix case: $(cat out)"

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
# (SOURCE_DATE_EPOCH makes the access times that extra fields hold its
# own, as reading the files may change them between runs.)
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX -X- -9r c3.zip tree
[ "$(wc -c <c3.zip)" -gt "$(wc -c <c1.zip)" ] || fail "-X- kept -X"
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX9 --strip-extra- -rp --paths c4.zip tree
cmp -s c3.zip c4.zip || fail "--strip-extra- or -p"
"$ZIP" -q --store c5.zip tree/a.txt
zipinfo c5.zip | grep -q stor || fail "--store compressed"
"$ZIP" -q c10.zip tree/a.txt tree/b.txt --i tree/a.txt  # Windows: ambiguous
[ "$(names c10.zip)" = tree/a.txt ] || fail "--i: $(names c10.zip)"
for opt in -q- -r- -d- -u- -f- -FS- -0- -9- -j- -nw- -@- -p- --quiet- \
           --delete- --exclude- --rec --compress --no-extra --store-only \
           --system-hidden --quiet=x -S -e --encr; do
    expect_status 16 "$ZIP" $opt c6.zip tree/a.txt
done
[ ! -e c6.zip ] || fail "an invalid option made an archive"
"$ZIP" -qr c7.zip tree -x-  # a list's value, not a negation
names c7.zip | grep -q tree/a.txt || fail "-x- excluded a.txt"

# Info-ZIP's two-letter options are matched before single letters, so
# that unsupported ones are rejected under their own names, while -h2 is
# help; -jj is not one of them, but -j twice
for opt in fd fz dd; do
    "$ZIP" -$opt c8.zip tree/a.txt 2>err && fail "-$opt succeeded"
    grep -q "short option '$opt' not supported" err || fail "-$opt: $(cat err)"
done
expect_status 16 "$ZIP" -qmm c8.zip tree/a.txt
"$ZIP" -mm c8.zip tree/a.txt 2>err && fail "-mm succeeded"
grep -q '(-mm not supported, Must_Match is -MM)$' err || fail "-mm: $(cat err)"
"$ZIP" -h2 >out
grep -q usage out || fail "-h2: $(cat out)"
for opt in -H '-?'; do  # Info-ZIP's aliases for -h
    "$ZIP" "$opt" c8.zip tree/a.txt >out || fail "$opt failed"
    grep -q usage out || fail "$opt: $(cat out)"
done
"$ZIP" -H- 2>err && fail "-H- succeeded"
grep -q "(option 'H' (help) not negatable)$" err || fail "-H-: $(cat err)"
"$ZIP" -qjj c8.zip tree/a.txt
[ "$(names c8.zip)" = a.txt ] || fail "-jj: $(names c8.zip)"

# As in Info-ZIP, one action (-u, -f, -d) may be given, once, while -FS
# is a flag that may be repeated, but not with an action
"$ZIP" -q c9.zip tree/a.txt tree/b.txt
cp c9.zip c9.orig
for opt in '-u -u' -uu '-f -f' -ff '-d -d' '-u -f' '-d -u' -df; do
    "$ZIP" $opt c9.zip tree/a.txt >out 2>&1 && fail "$opt succeeded"
    grep -q 'specify just one action' out || fail "$opt: $(cat out)"
done
"$ZIP" -u c9.zip -u tree/a.txt >out 2>&1 && fail "-u, -u later succeeded"
for opt in '-FS -u' '-f -FS' '-FS -d'; do
    "$ZIP" $opt c9.zip tree/a.txt >out 2>&1 && fail "$opt succeeded"
    grep -q "can't use -d, -f, -u, -U, or -g with filesync -FS)" out ||
        fail "$opt: $(cat out)"
done
cmp -s c9.zip c9.orig || fail "a repeated action changed the archive"
"$ZIP" -q -FS -FS c9.zip tree/a.txt
[ "$(names c9.zip)" = tree/a.txt ] || fail "-FS -FS: $(names c9.zip)"

# Determinism: -X output depends only on the tree, and SOURCE_DATE_EPOCH
# makes it independent of the time zone, before the epoch too. Zones are
# POSIX TZ strings, which need no time zone database, and earlier times
# are made east of UTC, where local times would show.
"$ZIP" -qX9r d1.zip tree
"$ZIP" -qX9r d2.zip tree
cmp -s d1.zip d2.zip || fail "-X output not deterministic"
SOURCE_DATE_EPOCH=1700000000 TZ=UTC0 "$ZIP" -qX9r e1.zip tree
SOURCE_DATE_EPOCH=1700000000 TZ=JST-9 "$ZIP" -qX9r e2.zip tree
cmp -s e1.zip e2.zip || fail "SOURCE_DATE_EPOCH output depends on TZ"
TZ=UTC0 touch -t 200001020304.05 tree/one
SOURCE_DATE_EPOCH=1700000000 TZ=JST-9 "$ZIP" -qX e3.zip tree/one tree/a.txt
zipinfo -T e3.zip | grep 'tree/one$' | grep -q 20000102.030406 ||
    fail "older time not kept (rounded up) in UTC: $(zipinfo -T e3.zip)"
zipinfo -T e3.zip | grep 'tree/a.txt$' | grep -q 20231114.221320 ||
    fail "newer time not clamped: $(zipinfo -T e3.zip)"
expect_status 16 env SOURCE_DATE_EPOCH=soon "$ZIP" -q e4.zip tree/one

# With an odd SOURCE_DATE_EPOCH, a clamped time rounds down rather than
# past it, as do times equal to it; earlier odd seconds round up
printf odd >odd.txt
TZ=UTC0 touch -t 202311142213.21 odd.txt  # 1700000001
SOURCE_DATE_EPOCH=1700000001 TZ=JST-9 "$ZIP" -qX e5.zip tree/a.txt odd.txt \
    tree/one
zipinfo -T e5.zip >out
grep 'tree/a.txt$' out | grep -q 20231114.221320 || fail "odd epoch: $(cat out)"
grep 'odd.txt$' out | grep -q 20231114.221320 || fail "odd epoch: $(cat out)"
grep 'tree/one$' out | grep -q 20000102.030406 || fail "odd epoch: $(cat out)"

# Without -X, the times of the UT extra field are clamped too, so that
# output does not depend on later changes to them, and the access time
# is the epoch, an earlier one too, as reading a file may change it
mkdir ut
printf new >ut/new
printf old >ut/old
printf past >ut/past
touch -t 203001010000 ut/new
TZ=UTC0 touch -m -t 202001010000 ut/old  # 1577836800, before the epoch
touch -a -t 203001010000 ut/old
TZ=UTC0 touch -t 201001010000 ut/past  # 1262304000, accessed then too
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -q ut1.zip ut/new ut/old ut/past
touch -t 203101010000 ut/new
touch -a -t 203101010000 ut/old
touch -a ut/past  # as reading it does on Linux, under relatime
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -q ut2.zip ut/new ut/old ut/past
cmp -s ut1.zip ut2.zip || fail "SOURCE_DATE_EPOCH without -X: times not clamped"
if [ -n "$PY" ]; then  # local modification and access, central times
    $PY -c 'import struct, sys
d = open(sys.argv[1], "rb").read()
def ut(x):
    while len(x) >= 4:
        i, n = struct.unpack("<HH", x[:4])
        if i == 0x5455:
            return struct.unpack("<%dI" % ((n-1)//4), x[5:4+n])
        x = x[4+n:]
    return ()
e = d.rfind(b"PK\5\6")
p = struct.unpack("<I", d[e+16:e+20])[0]
while d[p:p+4] == b"PK\1\2":
    c = struct.unpack("<IHHHHHHIIIHHHHHII", d[p:p+46])
    h = struct.unpack("<IHHHHHIIIHH", d[c[16]:c[16]+30])
    x = c[16] + 30 + h[9]
    print(d[p+46:p+46+c[10]].decode(), *ut(d[x:x+h[10]]),
          *ut(d[p+46+c[10]:p+46+c[10]+c[11]]))
    p += 46 + c[10] + c[11] + c[12]' ut1.zip >out
    printf '%s\n' 'ut/new 1700000000 1700000000 1700000000' \
        'ut/old 1577836800 1700000000 1577836800' \
        'ut/past 1262304000 1700000000 1262304000' >want
    cmp -s out want || fail "UT times under SOURCE_DATE_EPOCH: $(cat out)"
fi

# Times beyond the DOS range clamp to its ends. Departure: after 2107,
# where Info-ZIP wraps the year modulo 128 (2200 becomes 2072)
printf far >far.txt
if touch -t 220001010000 far.txt 2>/dev/null; then
    "$ZIP" -qX far.zip far.txt
    zipinfo -T far.zip | grep -q 21071231.235958 || fail "far: $(zipinfo -T far.zip)"
fi
touch -t 196001010000 far.txt  # as in Info-ZIP
"$ZIP" -qX far.zip far.txt
zipinfo -T far.zip | grep -q 19800101.000000 || fail "old: $(zipinfo -T far.zip)"

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

# ...even from a share root without its separator, which Info-ZIP adds
# to a directory before naming it
share=$(mktemp -d /tmp/zipshare.XXXXXX)
mkdir "$share/d"
printf s >"$share/d/s.txt"
st=0
"$ZIP" -qr share.zip "/$share" || st=$?
rm -rf "$share"
[ $st = 0 ] || fail "//host/share: status $st"
[ "$(names share.zip)" = "$(printf 'd/\nd/s.txt')" ] ||
    fail "//host/share: $(names share.zip)"

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

# Entries of "." are named without "./", as Info-ZIP names them, so a
# path below it named again is the same path, and one with ./ is not
(cd tree && "$ZIP" -qr ../dot1.zip . sub/random a.txt) || fail ". sub/random"
[ $(names dot1.zip | wc -l) = $(($(wc -l <want) - 1)) ] ||
    fail ". sub/random: $(names dot1.zip)"
(cd tree && expect_status 16 "$ZIP" -r ../dot2.zip . ./a.txt)
(cd tree && "$ZIP" -r ../dot3.zip . ./sub 2>../out) && fail ". ./sub"
grep -q 'second full name: sub/$' out || fail ". ./sub: $(cat out)"

# Departure: "./" without -r names nothing and is passed over, where
# Info-ZIP fails with an internal logic error (5, "empty name")
(cd tree && "$ZIP" -q ../dot4.zip ./ a.txt) || fail "./ without -r"
[ "$(names dot4.zip)" = a.txt ] || fail "./ without -r: $(names dot4.zip)"
(cd tree && echo ./ | expect_status 12 "$ZIP" ../dot5.zip -@)

# As in Info-ZIP, only the first repeat in order of names is reported,
# by the first two of its paths in order, in one indented warning
"$ZIP" -j dup6.zip tree/b.txt ./tree/b.txt tree/a.txt tree/sub ./tree/a.txt \
    ./tree/sub/../a.txt 2>out && fail "dup6.zip"
in='                     '
printf '%s\n' "zip warning:   first full name: ./tree/a.txt" \
    "$in second full name: ./tree/sub/../a.txt" \
    "${in}name in zip file repeated: a.txt" \
    "${in}this may be a result of using -j" "" \
    "zip error: Invalid command arguments (cannot repeat names in zip file)" \
    >want
cmp -s want out || fail "repeated names: $(cat out)"
# ...two different paths, a path given again counting once
"$ZIP" dup7.zip tree/a.txt ./tree/a.txt ./tree/a.txt 2>out && fail "dup7.zip"
grep -q 'first full name: \./tree/a\.txt$' out &&
    grep -q 'second full name: tree/a\.txt$' out ||
    fail "repeated names given again: $(cat out)"

# The archive never includes itself, which it knows by identity, though
# named otherwise than given (s.zip, ./s.zip) or through a hard link,
# while a copy of it is just a file
mkdir self
printf x >self/x
(cd self && "$ZIP" -qr s.zip . && cp s.zip copy.zip && "$ZIP" -qr ./s.zip .)
names self/s.zip >got
printf 'x\ncopy.zip\n' >want
cmp -s got want || fail "archive includes itself, or . names: $(cat got)"
if ln self/s.zip self/link.zip 2>/dev/null; then
    "$ZIP" -qr self/s.zip self
    names self/s.zip | grep -q link.zip && fail "archive added through a link"
fi

# Nor, as in Info-ZIP, another file named as the archive's path is
# given, as -j may name one, which leaves nothing to do if it is alone
mkdir build
printf artifact >build/dist.zip
"$ZIP" -qj dist.zip build/dist.zip tree/a.txt
[ "$(names dist.zip)" = a.txt ] || fail "named as the archive: $(names dist.zip)"
expect_status 12 "$ZIP" -jr dist.zip build
[ "$(names dist.zip)" = a.txt ] || fail "named as the archive (-r)"
expect_status 12 "$ZIP" -j dist build/dist.zip
mv build/dist.zip build/dist2.zip
expect_status 12 "$ZIP" -j dist2.zip build/dist2.zip
[ ! -e dist2.zip ] || fail "made an archive of a file named as it"
"$ZIP" -qj ./dist2.zip build/dist2.zip  # the path as given differs
[ "$(names dist2.zip)" = dist2.zip ] || fail "./: $(names dist2.zip)"
# Departure: they are left out before repeats are looked for, as
# excluded files are, so two are no repeat (Info-ZIP, after: 16)
mkdir build2
printf one >build/dist3.zip
printf two >build2/dist3.zip
"$ZIP" -qj dist3.zip build/dist3.zip build2/dist3.zip tree/a.txt ||
    fail "two named as the archive"
[ "$(names dist3.zip)" = a.txt ] ||
    fail "two named as the archive: $(names dist3.zip)"

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

# As in Info-ZIP, a list's first value is the next argument, whatever it
# is; later ones end at any argument starting with '-', a lone "-" (here
# streaming, rejected) included. An attached value is the whole list,
# even an empty one, which matches nothing. A missing list is an error.
printf d >./-dash
"$ZIP" -q dl1.zip tree/a.txt tree/b.txt ./-dash -x -dash tree/b.txt
[ "$(names dl1.zip)" = tree/a.txt ] || fail "-x -dash: $(names dl1.zip)"
"$ZIP" -q dl2.zip tree/a.txt tree/b.txt -i -- tree/b.txt
[ "$(names dl2.zip)" = tree/b.txt ] || fail "-i --: $(names dl2.zip)"
"$ZIP" -q dl3.zip tree/a.txt -x tree/b.txt - 2>err && fail "list then -"
grep -q '(streaming with - not supported)$' err || fail "list then -: $(cat err)"
"$ZIP" -q dl3.zip tree/a.txt -x @ tree/b.txt 2>err && fail "-x @"
grep -q '(missing file after @)$' err || fail "-x @: $(cat err)"
"$ZIP" -q dl4.zip tree/a.txt -x= tree/b.txt --exclude=
[ "$(names dl4.zip)" = "tree/a.txt
tree/b.txt" ] || fail "-x=: $(names dl4.zip)"
"$ZIP" -q dl5.zip --exclude= tree/a.txt
[ "$(names dl5.zip)" = tree/a.txt ] || fail "--exclude=: $(names dl5.zip)"
expect_status 12 "$ZIP" -q dl6.zip tree/a.txt -i=  # Departure, as above
"$ZIP" -q dl7.zip tree/a.txt -x 2>err && fail "-x without a list"
grep -q "(option 'x' (exclude files matching patterns) requires a value)$" \
    err || fail "-x without a list: $(cat err)"
"$ZIP" -q dl7.zip tree/a.txt --inc 2>err && fail "--inc without a list"
grep -q "'include' (include only files matching patterns) requires a value)$" \
    err || fail "--inc without a list: $(cat err)"
expect_status 16 "$ZIP" -q dl7.zip tree/a.txt --exclude
rm ./-dash
# Departure: -i matching nothing has nothing to do (12), where Info-ZIP
# writes an empty archive (0)
expect_status 12 "$ZIP" -r inone.zip tree -i '*.none'
[ ! -e inone.zip ] || fail "-i matching nothing made an archive"
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
"$ZIP" -qr x7.zip tree -x @missing.lst 2>err && fail "missing pattern file"
printf '%s\n' 'zip I/O error: No such file or directory' \
    "zip error: File not found or no read permission (x pattern file '@missing.lst')" >want
cmp -s err want || fail "missing pattern file: $(cat err)"
expect_status 18 "$ZIP" -qr x7.zip tree -x @missing.lst
expect_status 16 "$ZIP" -qr x7.zip tree -x@

# A directory as a pattern file is an empty list, as Info-ZIP reads it
# on POSIX: patterns that select nothing, nor need anything to select
mkdir -p atdir/d
printf x >atdir/f
(cd atdir && "$ZIP" -q ../atdir1.zip f -x@d -i @d) || fail "@dir list"
[ "$(names atdir1.zip)" = f ] || fail "@dir list: $(names atdir1.zip)"
"$ZIP" atdir2.zip -x@atdir/d >out 2>&1 && fail "@dir list, no paths"
[ "$(cat out)" = "
zip error: Nothing to do! (atdir2.zip)" ] || fail "@dir list: $(cat out)"
expect_status 12 "$ZIP" atdir2.zip -i@atdir/d

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

# A directory as standard input holds no names, as in Info-ZIP
mkdir -p stdir/d
printf x >stdir/f
(cd stdir && "$ZIP" -q ../stdir1.zip -@ f <d) || fail "-@ from a directory"
[ "$(names stdir1.zip)" = f ] || fail "-@ from a dir: $(names stdir1.zip)"
"$ZIP" stdir2.zip -@ <stdir/d >out 2>&1 && fail "-@ from a directory alone"
[ "$(cat out)" = "
zip error: Nothing to do! (stdir2.zip)" ] || fail "-@ from a dir: $(cat out)"
expect_status 12 "$ZIP" stdir2.zip -@ <stdir/d
expect_status 16 "$ZIP" stdir2.zip -x f -@ <stdir/d  # nothing to select from
# Departure: other read errors fail, where Info-ZIP ends the list there
expect_status 11 "$ZIP" stdir2.zip -@ stdir/f 0>>stdir/w
if [ -r /proc/self/mem ]; then  # Linux: EIO at offset 0
    expect_status 18 "$ZIP" stdir2.zip stdir/f -x@/proc/self/mem
fi

# Symbolic links: followed by default, stored as links with -y
if ln -s a.txt tree/link 2>/dev/null; then
    "$ZIP" -q l1.zip tree/link
    zipinfo l1.zip | grep -q '^-' || fail "link not followed"
    "$ZIP" -qy l2.zip tree/link
    zipinfo l2.zip | grep -q '^l' || fail "-y did not store a link"
    [ "$(unzip -p l2.zip tree/link)" = a.txt ] || fail "-y link target"
    verify l2.zip

    # As in Info-ZIP, links are always stored: no level bits, version 1.0,
    # and binary, even when the target would compress
    deep=../../../../../../../../../../../../../../../../etc/passwd
    ln -s $deep deep
    for level in -1 -6 -9; do
        "$ZIP" -qy $level l3.zip tree/link deep
        if [ -n "$PY" ]; then
            verify l3.zip
            grep -q '^deep 0 0x0 0x[0-9a-f]* 3 0 ' check.out &&
                grep -q '^tree/link 0 0x0 0x[0-9a-f]* 3 0 ' check.out ||
                fail "-y $level links: $(cat check.out)"
        fi
        zipinfo -v l3.zip | grep -q 'required to extract: *2' &&
            fail "-y $level link needs 2.0"
        [ "$(unzip -p l3.zip deep)" = $deep ] || fail "-y deep target"
    done
    rm deep
    ln -s . tree/sub/loop
    "$ZIP" -qr loop.zip tree/sub
    verify loop.zip
    "$ZIP" -qry loop2.zip tree/sub
    verify loop2.zip
    rm tree/link tree/sub/loop

    # Departure: a dangling link while recursing exits 18, where Info-ZIP
    # warns that the name is not matched (0)
    mkdir dl
    printf x >dl/f
    ln -s nowhere dl/dangling
    expect_status 18 "$ZIP" -r dl.zip dl
    [ "$(names dl.zip | tr '\n' ' ')" = "dl/ dl/f " ] ||
        fail "dangling link: $(names dl.zip)"
    # ...unless -x or -i leaves it out, so that nothing is lost (Info-ZIP
    # warns that the name is not matched, even then)
    "$ZIP" -qr dl2.zip dl -x dl/dangling 2>err ||
        fail "excluded dangling link: $(cat err)"
    [ ! -s err ] || fail "excluded dangling link: $(cat err)"
    expect_status 0 "$ZIP" -r dl3.zip dl -i dl/f
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
# Departure: with nothing newer, -u and -f exit 0 (Info-ZIP: 12)
expect_status 0 "$ZIP" -u u.zip tree/a.txt tree/one
expect_status 0 "$ZIP" -f u.zip tree/a.txt tree/b.txt
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

# Also as there, the last UT field decides, though without the time, and
# failing one, an old UX field: entries a day newer by these, and a day
# older by DOS time (or the reverse), than the file
if [ -n "$PY" ]; then
    printf one >ux.txt
    TZ=UTC0 touch -t 202311142213.20 ux.txt
    $PY -c 'import struct, time
t = 1700000000
def dos(s):
    g = time.gmtime(s)
    return ((g[0]-1980)<<25 | g[1]<<21 | g[2]<<16 | g[3]<<11 | g[4]<<5 |
            g[5]//2)
def ut(f, s=None):
    d = struct.pack("<B", f) + (struct.pack("<I", s) if s else b"")
    return b"UT" + struct.pack("<H", len(d)) + d
for kind, x, d in (("ux", b"UX\x08\x00" + struct.pack("<II", t, t+86400),
                    t-86400),
                   ("ut2", ut(1, t+86400) + ut(1, t-86400), t+86400),
                   ("ut0", ut(1, t+86400) + ut(0), t-86400)):
    n, m = b"ux.txt", dos(d)
    loc = struct.pack("<IHHHHHIIIHH", 0x04034b50, 10, 0, 0, m & 0xffff,
                      m >> 16, 0, 0, 0, len(n), 0) + n
    cen = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 10, 0, 0,
                      m & 0xffff, m >> 16, 0, 0, 0, len(n), len(x), 0, 0, 0,
                      0x81a40000, 0) + n + x
    end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(cen),
                      len(loc), 0)
    open("ux-%s.zip" % kind, "wb").write(loc + cen + end)'
    TZ=UTC0 "$ZIP" -u ux-ux.zip ux.txt | grep -q updating && fail "-u by UX"
    for kind in ut2 ut0; do
        TZ=UTC0 "$ZIP" -u ux-$kind.zip ux.txt | grep -q updating ||
            fail "-u by the last UT field ($kind)"
    done
fi

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

# With an odd epoch too: a file modified at it is not newer than its
# entry, and one modified a second later is
printf v1 >odd.txt
TZ=UTC0 touch -t 202311142213.21 odd.txt
SOURCE_DATE_EPOCH=1700000001 "$ZIP" -qX odd.zip odd.txt
cp odd.zip odd0.zip
SOURCE_DATE_EPOCH=1700000001 "$ZIP" -u odd.zip odd.txt >out
[ ! -s out ] && cmp -s odd.zip odd0.zip || fail "-u at an odd epoch: $(cat out)"
SOURCE_DATE_EPOCH=1700000001 "$ZIP" -FS odd.zip odd.txt >out
[ "$(cat out)" = "Archive is current" ] || fail "-FS at an odd epoch: $(cat out)"
printf v2 >odd.txt
TZ=UTC0 touch -t 202311142213.22 odd.txt
SOURCE_DATE_EPOCH=1700000001 "$ZIP" -qu odd.zip odd.txt
[ "$(unzip -p odd.zip odd.txt)" = v2 ] || fail "-u a second past an odd epoch"

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

# A file whose size alone changed, its time kept, is updated too
touch -r fs/a.txt fs.time
printf more >>fs/a.txt
touch -r fs.time fs/a.txt
"$ZIP" -FS -r fs.zip fs >out
progress out >got
printf 'updating: fs/a.txt\n' >want
cmp -s got want || fail "-FS of a changed size: $(cat out)"
extract_same fs.zip fs

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
selz 'updating: d1/a.txt updating: d1/b.log updating: d1/s/c.txt ' -u t.zip
grep -q 'file and directory with the same name: d1/b.log' err ||
    fail "file then directory: $(cat err)"
[ "$(unzip -p sel/t.zip d1/b.log)" = b ] || fail "file then directory kept"
(cd sel && expect_status 18 "$ZIP" -u t.zip)

# Departure: entry names select files only within the current
# directory, so that an untrusted archive cannot select any file beyond
# it: not by an absolute name, by .. components, or through a linked
# directory (Info-ZIP reads them all). Leading ./, as bsdtar writes it,
# leads nowhere else, so such entries are refreshed, as in Info-ZIP.
if [ -n "$PY" ]; then
    entries() {  # archive: each entry's name and contents
        $PY -c 'import sys, zipfile as z
for i in z.ZipFile(sys.argv[1]).infolist():
    print(i.filename, z.ZipFile(sys.argv[1]).read(i).decode())' "$1"
    }
    $PY -c 'import sys, zipfile as z
a = z.ZipFile(sys.argv[1], "w"); a.writestr(z.ZipInfo(sys.argv[2]), "x")' \
        abs.zip "$tmp/tree/a.txt"
    expect_status 12 "$ZIP" -f abs.zip
    [ "$(unzip -p abs.zip)" = x ] || fail "absolute entry name freshened"

    mkdir -p esc/top/work/sub/d esc/out
    printf secret >esc/top/secret
    printf outside >esc/out/key
    printf new >esc/top/work/sub/f
    printf new >esc/top/work/sub/d/g
    ln -s ../../../out esc/top/work/sub/link 2>/dev/null || :
    $PY -c 'import sys, zipfile as z
a = z.ZipFile(sys.argv[1], "w")
for n in sys.argv[2:]: a.writestr(z.ZipInfo(n), "" if n == "./" else "x")' \
        esc/top/work/sub/e.zip ./ ../../secret d/../../../secret \
        ./../../secret .//./f link/key d/g d/./g f
    cp esc/top/work/sub/e.zip esc/e0.zip
    (cd esc/top/work/sub && "$ZIP" -u e.zip >../../../log 2>&1) ||
        fail "-u of escaping names: $(cat esc/log)"
    grep -qx 'updating: \./ (stored 0%)' esc/log || fail "./: $(cat esc/log)"
    entries esc/top/work/sub/e.zip >got
    printf '%s\n' './ ' '../../secret x' 'd/../../../secret x' \
        './../../secret x' './/./f new' 'link/key x' 'd/g new' 'd/./g new' \
        'f new' >want
    cmp -s got want || fail "escaping names: $(cat got)"
    cp esc/e0.zip esc/top/work/sub/e.zip
    (cd esc/top/work/sub && "$ZIP" -f e.zip '*' >../../../log 2>&1) ||
        fail "-f of escaping names: $(cat esc/log)"
    entries esc/top/work/sub/e.zip >got
    cmp -s got want || fail "escaping names by pattern: $(cat got)"

    # Every entry of a name that entries select is refreshed, as Info-ZIP
    # examines the file of each entry it selects, while a path names only
    # the first (Info-ZIP's binary search finds any one of them)
    mkdir dups
    $PY -c 'import sys, warnings, zipfile as z
warnings.simplefilter("ignore")
a = z.ZipFile(sys.argv[1], "w")
for n, d in ("f", "one"), ("g", "g"), ("f", "two"):
    a.writestr(z.ZipInfo(n), d)' dups/d0.zip
    printf new >dups/f
    printf g >dups/g
    dups() {  # zip arguments, the archive dups/d.zip as d.zip
        cp dups/d0.zip dups/d.zip
        (cd dups && "$ZIP" -q "$@") || fail "duplicate names: $*"
        entries dups/d.zip | tr '\n' ' ' >got
    }
    for args in '-f d.zip' '-u d.zip' '-FS d.zip ?'; do
        set -f
        dups $args
        set +f
        [ "$(cat got)" = "f new g g f new " ] ||
            fail "$args over duplicates: $(cat got)"
    done
    dups -u d.zip f
    [ "$(cat got)" = "f new g g f two " ] ||
        fail "a path over duplicates: $(cat got)"
    # A -d name not on disk deletes every entry of that name, as a pattern
    # (which a name without wildcards is, though looked up by name)
    rm dups/f
    dups -d d.zip f
    [ "$(cat got)" = "g g " ] || fail "-d of duplicates: $(cat got)"
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
# Under -j, as Info-ZIP, a file on disk names the entry its name junked
# would, and a directory none, but -i and -x see the whole name
"$ZIP" -q dj.zip tree/sub/random lit/sx/r
"$ZIP" -qj dj.zip tree/sub/random lit/sx/r
"$ZIP" -qdj dj.zip tree/sub/random
names dj.zip >got
printf 'tree/sub/random\nlit/sx/r\nr\n' >want
cmp -s got want || fail "-dj of a name on disk: $(cat got)"
expect_status 12 "$ZIP" -dj dj.zip lit/sx
expect_status 12 "$ZIP" -dj dj.zip lit/sx/r -x 'lit/*'
"$ZIP" -qdj dj.zip lit/sx/r -x r
names dj.zip >got
printf 'tree/sub/random\nlit/sx/r\n' >want
cmp -s got want || fail "-dj -x: $(cat got)"

# A -d name that is a special file on disk marks nothing
mkdir ffd
printf x >ffd/fifo
(cd ffd && "$ZIP" -q ../ff.zip fifo)
if mkfifo fifo 2>/dev/null; then
    "$ZIP" -d ff.zip fifo >out 2>&1 && fail "-d of a FIFO name succeeded"
    grep -q 'ignoring FIFO (Named Pipe): fifo' out ||
        fail "-d of a FIFO: $(cat out)"
    [ "$(names ff.zip)" = fifo ] || fail "-d of a FIFO deleted the entry"
    rm fifo
fi

# Special files are left out with Info-ZIP's warnings, but for its advice
# to read a FIFO with -FI, which tugz does not support. Departure: so is
# a socket, which Info-ZIP takes for a file it cannot open (18).
mkdir spf
printf x >spf/f
if mkfifo spf/fifo 2>/dev/null; then
    "$ZIP" spf1.zip spf/fifo /dev/null spf/f >out 2>err || fail "special"
    printf '%s\n' 'zip warning: ignoring FIFO (Named Pipe): spf/fifo' \
        'zip warning: ignoring special file: /dev/null' >want
    cmp -s err want || fail "special files: $(cat err)"
    [ "$(names spf1.zip)" = spf/f ] || fail "special: $(names spf1.zip)"
    "$ZIP" -r spf2.zip spf >out 2>err || fail "special file in -r"
    [ "$(cat err)" = 'zip warning: ignoring FIFO (Named Pipe): spf/fifo' ] ||
        fail "special file in -r: $(cat err)"
    expect_status 12 "$ZIP" spf3.zip spf/fifo

    # ...even one named by an entry, which -u and -f, without paths, or
    # a pattern select, keeping the entry (Info-ZIP waits on the FIFO)
    mkdir spfe
    printf x >spfe/fifo
    printf y >spfe/f
    "$ZIP" -q spfe.zip spfe/fifo spfe/f
    rm spfe/fifo
    mkfifo spfe/fifo
    touch -t 203001010000 spfe/fifo spfe/f
    cp spfe.zip spfe.orig
    fifo_entry() {
        cp spfe.orig spfe.zip
        "$ZIP" "$@" >out 2>err || fail "FIFO by entry, $*"
        w='zip warning: ignoring FIFO (Named Pipe): spfe/fifo'
        [ "$(cat err)" = "$w" ] || fail "FIFO by entry, $*: $(cat err)"
        [ "$(unzip -p spfe.zip spfe/fifo)" = x ] &&
            [ "$(unzip -p spfe.zip spfe/f)" = y ] ||
            fail "FIFO by entry, $*: entries lost"
        ! grep -q fifo out || fail "FIFO by entry, $*: $(cat out)"
    }
    fifo_entry -u spfe.zip
    fifo_entry -f spfe.zip
    fifo_entry spfe.zip 'spfe/f*'
fi
if [ -n "$PY" ] && $PY -c 'import socket, sys
socket.socket(socket.AF_UNIX).bind(sys.argv[1])' spf/sock 2>/dev/null; then
    "$ZIP" spf4.zip spf/sock spf/f >out 2>err || fail "socket"
    [ "$(cat err)" = 'zip warning: ignoring special file: spf/sock' ] ||
        fail "socket: $(cat err)"
fi

# Copied entries keep their bytes and, as in Info-ZIP, their extra
# fields, as -X applies only to entries written
"$ZIP" -qr k1.zip tree
"$ZIP" -q k1.zip tree/a.txt
verify k1.zip
extract_same k1.zip tree
"$ZIP" -qX k1.zip tree/b.txt
"$ZIP" -qX -d k1.zip tree/one
verify k1.zip
if [ -n "$PY" ]; then
    grep -q '^tree/a.txt [08] 0x0 0x[0-9a-f]* 3 0 24 28 ' check.out &&
        grep -q '^tree/b.txt 0 0x0 0x[0-9a-f]* 3 0 0 0 ' check.out ||
        fail "-X merge extras: $(cat check.out)"

    # Also fields zip does not know, and replaced entries keep comments
    $PY -c 'import sys, zipfile as z
a = z.ZipFile(sys.argv[1], "w")
for n, c in ("tree/a.txt", b"note a"), ("tree/b.txt", b"note b"):
    i = z.ZipInfo(n)
    i.extra = b"\xfe\xca\x02\x00hi"
    i.comment = c
    a.writestr(i, "old")' cm.zip
    "$ZIP" -qX cm.zip tree/a.txt tree/one
    verify cm.zip
    grep -q '^tree/a.txt [08] 0x0 0x[0-9a-f]* 3 0 0 0 note a$' check.out &&
        grep -q '^tree/b.txt 0 0x0 0x[0-9a-f]* 3 0 6 6 note b$' check.out &&
        grep -q '^tree/one 0 0x0 0x[0-9a-f]* 3 0 0 0 $' check.out ||
        fail "kept fields and comments: $(cat check.out)"

    # A copied entry whose extra fields leave no room for the Zip64 field
    # it needs is refused, rather than written with a wrapped length
    $PY -c 'import struct, sys
n = b"big"
x = b"\xfe\xca" + struct.pack("<H", 65516) + bytes(65516)
z = struct.pack("<HHQ", 1, 8, 5 << 30)  # its size: 5 GiB
loc = struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, 0, 0, 0, 33, 0, 1,
                  0xffffffff, len(n), len(x)) + n + x + b"x"
cen = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 45, 0, 0, 0,
                  33, 0, 1, 0xffffffff, len(n), len(z), 0, 0, 0, 0, 0)
cen += n + z
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(cen), len(loc),
                  0)
open(sys.argv[1], "wb").write(loc + cen + end)' nofit.zip
    cp nofit.zip nofit0.zip
    "$ZIP" nofit.zip tree/a.txt >out 2>&1 && fail "no room for Zip64"
    grep -q 'structure invalid (big: no room' out || fail "no room: $(cat out)"
    cmp -s nofit.zip nofit0.zip || fail "a refused copy changed the archive"

    # A local header that disagrees with the central one gets Info-ZIP's
    # warnings on copying, in its words and order, but for the CRC that
    # a descriptor gives, and is written anew from the central header
    $PY -c 'import struct, sys, zlib
loc = cen = b""
for cn, ln, lv, lf, lc, f in ((b"m/a.txt", b"m/b.txt", 20, 0x800, 1, 0),
                              (b"m/c.txt", b"m/c.txt", 10, 8, 0, 8)):
    d = cn * 3
    c, o = zlib.crc32(d), len(loc)
    loc += struct.pack("<IHHHHHIIIHH", 0x04034b50, lv, lf, 0, 0, 0x5021, lc,
                       len(d), len(d), len(ln), 0) + ln + d
    if f:
        loc += struct.pack("<IIII", 0x08074b50, c, len(d), len(d))
    cen += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 10, f, 0, 0,
                       0x5021, c, len(d), len(d), len(cn), 0, 0, 0, 0,
                       0x81a40000, o) + cn
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 2, 2, len(cen), len(loc), 0)
open(sys.argv[1], "wb").write(loc + cen + end)' mm.zip
    "$ZIP" mm.zip tree/b.txt >out 2>err || fail "mismatched local headers"
    for what in 'Version Needed To Extract' 'Entry Flag' 'Entry CRC' \
                'Entry name'; do
        echo "zip warning: Local $what does not match CD: m/a.txt"
    done >want
    cmp -s err want || fail "mismatched local headers: $(cat err)"
    verify mm.zip
    [ "$(names mm.zip | tr '\n' ' ')" = "m/a.txt m/c.txt tree/b.txt " ] ||
        fail "mismatched local headers: $(names mm.zip)"

    # As in Info-ZIP, an entry without a name makes the archive invalid
    $PY -c 'import struct, sys
loc = cen = b""
for n in b"", b"ok.txt":
    o = len(loc)
    loc += struct.pack("<IHHHHHIIIHH", 0x04034b50, 10, 0, 0, 0, 0x5021, 0,
                       0, 0, len(n), 0) + n
    cen += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 10, 0, 0, 0,
                       0x5021, 0, 0, 0, len(n), 0, 0, 0, 0, 0x81a40000, o) + n
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 2, 2, len(cen), len(loc), 0)
open(sys.argv[1], "wb").write(loc + cen + end)' noname.zip
    cp noname.zip noname0.zip
    "$ZIP" noname.zip tree/a.txt >out 2>&1 && fail "unnamed entry accepted"
    printf '%s\n' 'zip warning: zero-length name for entry #1' '' \
        'zip error: Zip file structure invalid (noname.zip)' >want
    cmp -s out want || fail "unnamed entry: $(cat out)"
    cmp -s noname.zip noname0.zip || fail "an unnamed entry changed the archive"

    # Archives made by other tools: entries a streaming writer gave data
    # descriptors lose them when copied, their sizes now known, and the
    # comments of entries and of the archive are kept
    $PY -c 'import sys, zipfile as z
class Stream:  # unseekable, so that zipfile writes data descriptors
    def __init__(self, f): self.f = f
    def write(self, b): return self.f.write(b)
    def flush(self): self.f.flush()
with open(sys.argv[1], "wb") as f, z.ZipFile(Stream(f), "w") as a:
    a.comment = b"archive note"
    for n, m in ("s/one.txt", z.ZIP_DEFLATED), ("s/two.txt", z.ZIP_STORED):
        i = z.ZipInfo(n, (2020, 1, 1, 0, 0, 0))
        i.compress_type = m
        i.comment = n.encode()
        a.writestr(i, n * 100)' dd.zip
    verify dd.zip
    grep -q '^s/one.txt 8 0x8 ' check.out ||
        fail "no descriptors: $(cat check.out)"
    "$ZIP" -q dd.zip tree/a.txt
    verify dd.zip
    grep -q '^s/one.txt 8 0x0 .* s/one.txt$' check.out &&
        grep -q '^s/two.txt 0 0x0 .* s/two.txt$' check.out ||
        fail "copied descriptor entries: $(cat check.out)"
    $PY -c 'import sys
sys.exit(b"PK\7\10" in open(sys.argv[1], "rb").read())' dd.zip ||
        fail "a data descriptor was copied"
    [ "$(unzip -z dd.zip | sed 1d)" = "archive note" ] ||
        fail "archive comment: $(unzip -z dd.zip)"
    "$ZIP" -qd dd.zip s/two.txt
    [ "$(unzip -z dd.zip | sed 1d)" = "archive note" ] ||
        fail "archive comment after -d: $(unzip -z dd.zip)"

    # Encrypted entries with descriptors keep them, as in Info-ZIP, since
    # with traditional encryption the check byte of the header is then
    # the time's, not the CRC's (zip cannot encrypt, so this one is made
    # here)
    $PY -c 'import struct, sys, zlib
def crc(k, b):  # the CRC-32 step of the cipher
    return zlib.crc32(bytes([b]), k ^ 0xffffffff) ^ 0xffffffff
k = [0x12345678, 0x23456789, 0x34567890]
def update(b):
    k[0] = crc(k[0], b)
    k[1] = ((k[1] + (k[0] & 0xff)) * 134775813 + 1) & 0xffffffff
    k[2] = crc(k[2], k[1] >> 24)
def encrypt(data):
    r = bytearray()
    for b in data:
        t = k[2] & 0xffff | 2
        r.append(b ^ (t * (t ^ 1) >> 8 & 0xff))
        update(b)
    return bytes(r)
for b in b"secret":
    update(b)
n, d, time, date = b"enc.txt", b"hidden text", 0x6000, 0x5021
x = encrypt(bytes(range(11)) + bytes([time >> 8]) + d)
c = zlib.crc32(d)
loc = struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 9, 0, time, date, 0, 0, 0,
                  len(n), 0) + n + x
loc += struct.pack("<IIII", 0x08074b50, c, len(x), len(d))
cen = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 20, 9, 0, time,
                  date, c, len(x), len(d), len(n), 0, 0, 0, 0, 0x81a40000, 0)
cen += n
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(cen), len(loc), 0)
open(sys.argv[1], "wb").write(loc + cen + end)' enc.zip
    unzip -P secret -tqq enc.zip >/dev/null 2>&1 || fail "enc.zip invalid"
    "$ZIP" -q enc.zip tree/a.txt
    unzip -P secret -tqq enc.zip >/dev/null 2>&1 ||
        fail "copied encrypted entry: $(unzip -P secret -t enc.zip 2>&1)"
    [ "$(unzip -P secret -p enc.zip enc.txt)" = "hidden text" ] ||
        fail "copied encrypted entry contents"
fi

# Copied entries' Zip64 fields are made anew, as their sizes need, from
# whatever their writer gave them, even for small sizes: in the local
# header, Info-ZIP's for a stream (sizes to follow in a descriptor), or
# both sizes; in the central one, both sizes, all three fields, just the
# offset, or fields that need none. Their other fields are kept in
# order, and the two headers agree.
if [ -n "$PY" ]; then
    $PY -c 'import struct, sys, zlib
M = 0xffffffff
def ext(i, d):
    return struct.pack("<HH", i, len(d)) + d
def z64(*v):
    return ext(1, struct.pack("<%dQ" % len(v), *v))
x = ext(0x5455, struct.pack("<BI", 1, 1577836800)) + ext(0xcafe, b"hi")
loc = cen = b""
for kind in "stream", "sizes", "all", "offset", "literal":
    n = b"z/%s.txt" % kind.encode()
    d = n * 40
    m = 0 if kind in ("sizes", "offset") else 8
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    r = c.compress(d) + c.flush() if m else d
    crc, s, u, o = zlib.crc32(d), len(r), len(d), len(loc)
    f = 8 if kind == "stream" else 0
    lh = {"stream":  (0, M, M, z64(0, 0) + x),
          "literal": (crc, s, u, x + z64(u, s))}.get(kind,
                     (crc, M, M, z64(u, s) + x))
    ch = {"stream":  (s, u, o, x),
          "sizes":   (M, M, o, z64(u, s) + x),
          "all":     (M, M, M, z64(u, s, o) + x),
          "offset":  (s, u, M, z64(o) + x),
          "literal": (s, u, o, x + z64(u, s, o))}[kind]
    loc += struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, f, m, 0, 0x5021,
                       *lh[:3], len(n), len(lh[3])) + n + lh[3] + r
    if f:
        loc += struct.pack("<IIQQ", 0x08074b50, crc, s, u)
    cen += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 45, f, m,
                       0, 0x5021, crc, ch[0], ch[1], len(n), len(ch[3]), 0,
                       0, 0, 0x81a40000, ch[2]) + n + ch[3]
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 5, 5, len(cen), len(loc), 0)
open(sys.argv[1], "wb").write(loc + cen + end)' z64.zip
    verify z64.zip
    "$ZIP" -qX z64.zip tree/b.txt
    verify z64.zip
    $PY -c 'import struct, sys
d = open(sys.argv[1], "rb").read()
def ids(x):
    r = []
    while len(x) >= 4:
        i, n = struct.unpack("<HH", x[:4])
        r.append("%x:%d" % (i, n))
        x = x[4+n:]
    return ",".join(r) or "-"
e = d.rfind(b"PK\5\6")
n, _, p = struct.unpack("<HII", d[e+10:e+20])
for _ in range(n):
    c = struct.unpack("<IHHHHHHIIIHHHHHII", d[p:p+46])
    name = d[p+46:p+46+c[10]]
    cx = d[p+46+c[10]:p+46+c[10]+c[11]]
    o = c[16]
    h = struct.unpack("<IHHHHHIIIHH", d[o:o+30])
    lx = d[o+30+h[9]:o+30+h[9]+h[10]]
    if h[0] != 0x04034b50 or h[1:9] != c[2:10] or d[o+30:o+30+h[9]] != name:
        sys.exit("%s: local header differs" % name)
    if 0xffffffff in (c[8], c[9], c[16], h[7], h[8]):
        sys.exit("%s: saturated field" % name)
    print(name.decode(), c[2], hex(c[3]), ids(lx), ids(cx))
    p += 46 + c[10] + c[11] + c[12]' z64.zip >out 2>&1 ||
        fail "copied Zip64 entries: $(cat out)"
    for kind in stream sizes all offset literal; do
        echo "z/$kind.txt 45 0x0 5455:5,cafe:2 5455:5,cafe:2"
    done >want
    echo 'tree/b.txt 10 0x0 - -' >>want
    cmp -s out want || fail "copied Zip64 entries: $(cat out)"
fi

# As in Info-ZIP, a file replaces an entry whose stored name, in a code
# page (here CP437, as Windows tools write it), it matches only by the
# entry's Info-ZIP Unicode path field, if that is for the stored name;
# the entry is then written under its Unicode name, flagged UTF-8
if printf u >"$u" 2>/dev/null && [ -n "$PY" ]; then
    for crc in good stale; do
        $PY -c 'import struct, sys, zlib
n = b"caf\x82.txt"
c = zlib.crc32(n) if sys.argv[2] == "good" else 0
x = b"up" + struct.pack("<HBI", 14, 1, c) + "café.txt".encode()
d = b"old"
loc = struct.pack("<IHHHHHIIIHH", 0x04034b50, 10, 0, 0, 0, 0x5021,
                  zlib.crc32(d), len(d), len(d), len(n), len(x)) + n + x + d
cen = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0xb14, 10, 0, 0, 0,
                  0x5021, zlib.crc32(d), len(d), len(d), len(n), len(x), 0,
                  0, 0, 0x20, 0) + n + x
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(cen), len(loc), 0)
open(sys.argv[1], "wb").write(loc + cen + end)' up-$crc.zip $crc
    done
    for mode in '' -u -f -FS; do
        cp up-good.zip up.zip
        "$ZIP" $mode up.zip "$u" >out
        verify up.zip
        [ "$(wc -l <check.out | tr -d ' ')" = 1 ] &&
            grep -q "^$u [08] 0x800 " check.out ||
            fail "$mode by Unicode path: $(cat check.out)"
        progress out | grep -qx "[a-z]*: $u" ||
            fail "$mode by Unicode path: $(cat out)"
    done
    cp up-good.zip up.zip
    "$ZIP" -d up.zip "$u" >out 2>&1
    grep -qx "deleting: $u" out || fail "-d by Unicode path: $(cat out)"
    cp up-stale.zip up.zip
    "$ZIP" -q up.zip "$u"
    verify up.zip
    [ "$(wc -l <check.out | tr -d ' ')" = 2 ] ||
        fail "stale Unicode path matched: $(cat check.out)"
fi

# Departure: a file matches by Unicode name only an entry that no file
# matches by stored name, so that one entry for two files (x.txt, by its
# stored name, and y.txt, by its Unicode path field) keeps both: x.txt
# replaces it, and y.txt is added (Info-ZIP, with Unicode support, lets
# the last replace it, losing the other)
if [ -n "$PY" ]; then
    $PY -c 'import struct, sys, zlib
n, u, d = b"x.txt", b"y.txt", b"old"
x = b"up" + struct.pack("<HBI", 5 + len(u), 1, zlib.crc32(n)) + u
loc = struct.pack("<IHHHHHIIIHH", 0x04034b50, 10, 0, 0, 0, 0x5021,
                  zlib.crc32(d), len(d), len(d), len(n), len(x)) + n + x + d
cen = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 10, 0, 0, 0,
                  0x5021, zlib.crc32(d), len(d), len(d), len(n), len(x), 0,
                  0, 0, 0x81a40000, 0) + n + x
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(cen), len(loc), 0)
open(sys.argv[1], "wb").write(loc + cen + end)' up2.zip
    printf 'new x' >x.txt
    printf 'new y' >y.txt
    for files in "x.txt y.txt" "y.txt x.txt"; do
        cp up2.zip up.zip
        "$ZIP" up.zip $files >out
        verify up.zip
        printf 'updating: x.txt\n  adding: y.txt\n' >want
        progress out | cmp -s - want || fail "$files, one entry: $(cat out)"
        [ "$(cut -d' ' -f1 check.out | tr '\n' ' ')" = "x.txt y.txt " ] &&
            [ "$(unzip -p up.zip x.txt)" = "new x" ] &&
            [ "$(unzip -p up.zip y.txt)" = "new y" ] ||
            fail "$files, one entry: $(cat check.out)"
    done
    # ...while a pattern refreshes every entry of a name that it selects
    mkdir upd
    cp x.txt y.txt upd/
    printf 'new f' >upd/f
    $PY -c 'import struct, sys, zlib
loc = cen = b""
for n, d in (b"x.txt", b"old"), (b"f", b"one"), (b"f", b"two"):
    u = b"y.txt"
    x = b"up" + struct.pack("<HBI", 5 + len(u), 1, zlib.crc32(n)) + u
    x = x if n == b"x.txt" else b""
    c = zlib.crc32(d)
    cen += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 10, 0, 0,
                       0, 0x5021, c, len(d), len(d), len(n), len(x), 0, 0,
                       0, 0x81a40000, len(loc)) + n + x
    loc += struct.pack("<IHHHHHIIIHH", 0x04034b50, 10, 0, 0, 0, 0x5021, c,
                       len(d), len(d), len(n), len(x)) + n + x + d
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 3, 3, len(cen), len(loc), 0)
open(sys.argv[1], "wb").write(loc + cen + end)' up3.zip
    for files in "x.txt y.txt" "y.txt x.txt"; do
        cp up3.zip upd/up.zip
        (cd upd && "$ZIP" -u up.zip $files '?' >../out) ||
            fail "$files ?, one entry and two: $(cat out)"
        verify upd/up.zip
        printf '%s\n' 'updating: x.txt' 'updating: f' 'updating: f' \
            '  adding: y.txt' >want
        progress out | cmp -s - want ||
            fail "$files ?, one entry and two: $(cat out)"
        [ "$(entries upd/up.zip | tr '\n' ' ')" = \
          "x.txt new x f new f f new f y.txt new y " ] ||
            fail "$files ?, one entry and two: $(entries upd/up.zip)"
    done
fi

# Data before the first entry that offsets account for, such as a
# self-extractor's stub after zip -A, is kept, as in Info-ZIP, and the
# offsets stay absolute; offsets that do not account for it are refused,
# as by Info-ZIP, and with a warning saying so. Departure: before any
# work, even when no entry would be copied (Info-ZIP fails on copying
# one, warning that it "did not find" it). Here the stub precedes an
# empty archive's central directory (offset 28).
printf '#!/bin/sh\necho stub; exit 0\n' >stub
printf one >sx1.txt
printf two >sx2.txt
"$ZIP" -q plain.zip sx1.txt
cat stub plain.zip >sfx0.zip
cp sfx0.zip sfx0.orig
expect_status 3 "$ZIP" sfx0.zip sx2.txt
expect_status 3 "$ZIP" -d sfx0.zip sx1.txt
"$ZIP" sfx0.zip sx2.txt >out 2>&1 && fail "unadjusted stub accepted"
w='zip warning: offsets do not account for data before the archive'
printf '%s\n' "$w" '' 'zip error: Zip file structure invalid (sfx0.zip)' >want
cmp -s out want || fail "unadjusted stub: $(cat out)"
cmp -s sfx0.zip sfx0.orig || fail "a refused stub changed the archive"
{ cat stub; printf 'PK\005\006\0\0\0\0\0\0\0\0\0\0\0\0\034\0\0\0\0\0'; } >sfx.zip
"$ZIP" -q sfx.zip sx1.txt
for mode in -q -qX -qFS; do
    "$ZIP" $mode sfx.zip sx2.txt sx1.txt
done
"$ZIP" -qd sfx.zip sx2.txt
touch -t 203001010000 sx1.txt
"$ZIP" -qf sfx.zip
verify sfx.zip
[ "$(names sfx.zip)" = sx1.txt ] || fail "self-extractor: $(names sfx.zip)"
head -c 28 sfx.zip | cmp -s - stub || fail "self-extractor stub lost"
[ "$(sh sfx.zip)" = stub ] || fail "self-extractor stub does not run"
"$ZIP" -qd sfx.zip '*'
[ "$(wc -c <sfx.zip | tr -d ' ')" = 50 ] || fail "emptied self-extractor"

# Emptied, its end record gives its directory offset 0, as Info-ZIP's
# does, by which UnZip finds it empty, past the stub, rather than
# corrupt; and either form gets entries again after the stub
tail -c 6 sfx.zip | od -An -tx1 | tr -d ' \n' >got
[ "$(cat got)" = 000000000000 ] ||
    fail "emptied self-extractor offset: $(cat got)"
set +e
unzip -t sfx.zip >out 2>&1
st=$?
set -e
[ $st = 1 ] && grep -q 'zipfile is empty' out ||
    fail "emptied self-extractor, unzip: $st $(cat out)"
{ cat stub; printf 'PK\005\006\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0'; } >sfx0e.zip
for arc in sfx.zip sfx0e.zip; do
    "$ZIP" -q $arc sx2.txt
    verify $arc
    [ "$(names $arc)" = sx2.txt ] || fail "refilled $arc: $(names $arc)"
    head -c 28 $arc | cmp -s - stub || fail "refilled $arc: stub lost"
done

# Likewise a Python zipapp's #! line
if [ -n "$PY" ]; then
    mkdir app
    printf 'print("hello from app")\n' >app/__main__.py
    $PY -m zipapp app -p '/usr/bin/env python3' -o app.pyz
    "$ZIP" -q app.pyz tree/b.txt
    verify app.pyz
    [ "$(head -c 2 app.pyz)" = '#!' ] || fail "zipapp's #! line lost"
    [ "$($PY app.pyz)" = 'hello from app' ] || fail "zipapp does not run"
fi

# End records as other writers make them: a count of entries on this
# disk that differs from the total (on one disk) is no split archive, as
# in Info-ZIP; a small archive may have Zip64 records beside an end
# record of real values, and a fault in those is its own, not data
# before the archive
if [ -n "$PY" ]; then
    $PY -c 'import struct, sys, zlib
loc = cen = b""
for n, d in (b"e/a.txt", b"alpha"), (b"e/b.txt", b"bravo"):
    c, o = zlib.crc32(d), len(loc)
    loc += struct.pack("<IHHHHHIIIHH", 0x04034b50, 10, 0, 0, 0, 0x5021, c,
                       len(d), len(d), len(n), 0) + n + d
    cen += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 0x31e, 10, 0, 0, 0,
                       0x5021, c, len(d), len(d), len(n), 0, 0, 0, 0,
                       0x81a40000, o) + n
def write(name, ndisk=2, total=1, cdoff=len(loc), z64=True):
    out = loc + cen
    if z64:
        rec = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 0x31e, 45, 0, 0, 2,
                          2, len(cen), cdoff)
        out += rec + struct.pack("<IIQI", 0x07064b50, 0, len(out), total)
    out += struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, ndisk, 2, len(cen),
                       len(loc), 0)
    open(name, "wb").write(out)
write("nd0.zip", ndisk=0, z64=False)
write("r64.zip")
write("r64split.zip", total=2)
write("r64bad.zip", cdoff=len(loc)+1)'
    for f in nd0 r64; do
        "$ZIP" -q $f.zip tree/a.txt || fail "end records of $f.zip"
        verify $f.zip
        [ "$(names $f.zip | tr '\n' ' ')" = "e/a.txt e/b.txt tree/a.txt " ] ||
            fail "end records of $f.zip: $(names $f.zip)"
    done
    "$ZIP" r64split.zip tree/a.txt >out 2>&1 && fail "split Zip64 accepted"
    printf '\nzip error: Split archives not supported (r64split.zip)\n' >want
    cmp -s out want || fail "split Zip64: $(cat out)"
    "$ZIP" r64bad.zip tree/a.txt >out 2>&1 && fail "bad Zip64 accepted"
    printf '\nzip error: Zip file structure invalid (r64bad.zip)\n' >want
    cmp -s out want || fail "bad Zip64: $(cat out)"
fi

# Errors and warnings
expect_status 12 "$ZIP" nothing.zip missing
[ ! -e nothing.zip ] || fail "created an archive with nothing to do"
# Under -r without patterns, Info-ZIP's error suggests the paths as -i
# patterns of ".", in the arguments with options first
env ZIPOPT=-9 "$ZIP" -r nothing.zip missing -q -- missing2 2>err &&
    fail "-r with nothing to do"
[ "$(cat err)" = "
zip error: Nothing to do! (try: zip -9 -r -q nothing.zip . -i missing -- missing2)" ] ||
    fail "-r with nothing to do: $(cat err)"
"$ZIP" -qr nothing.zip missing -x missing2 2>err && fail "-r -x, nothing to do"
[ "$(cat err)" = "
zip error: Nothing to do! (nothing.zip)" ] || fail "-r -x, nothing to do: $(cat err)"
expect_status 0 "$ZIP" some.zip missing tree/a.txt
expect_status 16 "$ZIP" dup.zip tree/a.txt ./tree/a.txt
expect_status 16 "$ZIP" -K bad.zip tree/a.txt
expect_status 16 "$ZIP" -e bad.zip tree/a.txt
expect_status 16 "$ZIP" --bogus bad.zip tree/a.txt
expect_status 16 "$ZIP" - tree/a.txt

# A "-" path, which Info-ZIP reads from standard input, is rejected as
# streaming, but under -d, as there, it names the entry that makes
printf x >./-
"$ZIP" -q de.zip ./- tree/a.txt
rm ./-
expect_status 16 "$ZIP" de.zip tree/b.txt -
expect_status 16 "$ZIP" -u de.zip -
expect_status 12 "$ZIP" -d de.zip - -x -  # -x applies, as to any name
"$ZIP" de.zip - -d >out || fail "-d -: $(cat out)"
[ "$(cat out)" = "deleting: -" ] || fail "-d -: $(cat out)"
[ "$(names de.zip)" = tree/a.txt ] || fail "-d -: $(names de.zip)"
"$ZIP" -d de.zip - 2>err && fail "-d - without the entry succeeded"
grep -q 'not matched' err && fail "-d - warned: $(cat err)"

# As in Info-ZIP, -x and -i patterns need something to select from:
# paths, even from -@, or for -u and -f the archive's entries
"$ZIP" -q ns.zip tree/a.txt
cp ns.zip ns.orig
for mode in -q -qd -qFS; do
    "$ZIP" $mode ns.zip -x '*.tmp' >out 2>&1 && fail "$mode -x, no paths"
    grep -q 'nothing to select from' out || fail "$mode -x: $(cat out)"
done
printf '\n' | "$ZIP" -@ ns.zip -i '*.txt' >out 2>&1 && fail "-@ -i, no paths"
grep -q 'nothing to select from' out || fail "-@ -i: $(cat out)"
expect_status 16 "$ZIP" -x '*.tmp'
expect_status 16 "$ZIP" -d ns-gone.zip -x '*.tmp'
cmp -s ns.zip ns.orig || fail "nothing to select from changed the archive"
expect_status 0 "$ZIP" -qu ns.zip -x '*.tmp'
echo junk >junk.zip
head -c 100 t.zip >trunc.zip
for arc in junk.zip trunc.zip; do  # left as they were
    cp $arc bad.orig
    for mode in -q -qu -qd -qFS; do
        expect_status 3 "$ZIP" $mode $arc tree/a.txt
    done
    cmp -s $arc bad.orig || fail "a failed merge changed $arc"
done

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

# Links at the archive path survive: the archive is replaced where they
# lead, as Info-ZIP updates it through them, whether a link is relative
# (to its directory), absolute, or leads to another. The archive is not
# added to itself through them. A loop cannot be written (15).
mkdir -p al/dist al/store
"$ZIP" -q al/store/r.zip tree/a.txt
if ln -s ../store/r.zip al/dist/rel.zip 2>/dev/null; then
    ln -s "$(pwd)/al/dist/rel.zip" al/abs.zip
    "$ZIP" -q al/dist/rel.zip tree/b.txt
    "$ZIP" -qr al/abs.zip tree/one al
    [ -L al/dist/rel.zip ] && [ -L al/abs.zip ] || fail "archive link replaced"
    names al/store/r.zip >got
    printf '%s\n' tree/a.txt tree/b.txt tree/one al/ al/dist/ al/store/ >want
    cmp -s got want || fail "archive through links: $(cat got)"
    case "$(ls al al/dist al/store)" in
    *zi[0-9]*) fail "temporary file left beside an archive link";;
    esac

    # Departure: a dangling link gets its target created, where Info-ZIP
    # leaves an empty file there and replaces the link with the archive
    ln -s ../store/new.zip al/dist/dangling.zip
    "$ZIP" -q al/dist/dangling.zip tree/a.txt
    [ -L al/dist/dangling.zip ] || fail "dangling archive link replaced"
    [ "$(names al/store/new.zip)" = tree/a.txt ] ||
        fail "dangling archive link: $(names al/store/new.zip)"
    ln -s loop.zip al/loop.zip
    "$ZIP" al/loop.zip tree/a.txt 2>err && fail "archive link loop succeeded"
    grep -q 'Could not create output file (al/loop.zip)' err ||
        fail "archive link loop: $(cat err)"
    [ -L al/loop.zip ] || fail "archive link loop replaced"

    # So cannot an empty link, which macOS and the BSDs allow, and which
    # leads nowhere: not to its directory, nor to an empty path
    for e in al/empty.zip empty.zip; do
        ln -s '' $e 2>/dev/null || break  # Linux refuses one
        printf '%s\n' 'zip I/O error: No such file or directory' \
            "zip error: Could not create output file ($e)" >want
        "$ZIP" $e tree/a.txt >out 2>err && fail "empty link $e succeeded"
        [ ! -s out ] && cmp -s err want || fail "empty link $e: $(cat out err)"
        [ -L $e ] || fail "empty link $e replaced"
        rm $e
    done

    # A chain as long as the system follows (Linux: 40 links) is
    # followed too, and one link more is a loop, with its reason
    mkdir al/chain
    "$ZIP" -q al/chain/l0.zip tree/a.txt
    n=0
    while [ -e al/chain/l$n.zip ] && [ $n -lt 100 ]; do
        ln -s l$n.zip al/chain/l$((n+1)).zip
        n=$((n + 1))
    done
    "$ZIP" -q al/chain/l$((n-1)).zip tree/b.txt ||
        fail "archive through $((n-1)) links"
    [ "$(names al/chain/l0.zip | tr '\n' ' ')" = "tree/a.txt tree/b.txt " ] ||
        fail "archive through $((n-1)) links: $(names al/chain/l0.zip)"
    "$ZIP" al/chain/l$n.zip tree/one 2>err && fail "$n links succeeded"
    grep -q 'zip I/O error: ..' err || fail "$n links: $(cat err)"
fi

# Departure: a hard-linked archive is replaced by a new file, which its
# other names do not share (Info-ZIP writes the new archive into it)
if "$ZIP" -q hl1.zip tree/a.txt && ln hl1.zip hl2.zip 2>/dev/null; then
    "$ZIP" -q hl1.zip tree/b.txt
    [ "$(names hl2.zip)" = tree/a.txt ] || fail "hard link: $(names hl2.zip)"
fi

# A failed read is told from a failed open (Linux: reading this file at
# offset 0 fails), after its reason, as Info-ZIP's perror gives it
if [ -r /proc/self/mem ]; then
    "$ZIP" mem.zip /proc/self/mem >out 2>err && fail "read error succeeded"
    printf '%s\n' 'zip warning: Input/output error' \
        'zip warning: could not read input file: proc/self/mem' >want
    head -n 2 err | cmp -s - want || fail "read error: $(cat err)"
fi

# A failed write stops at its entry, whose progress line it ends without
# a result, with Info-ZIP's error, leaving no archive or temporary file,
# or the archive as it was
mkdir wf
head -c 3000000 /dev/urandom >wf/big
printf small >wf/small
"$ZIP" -q wf/u.zip wf/small
cp wf/u.zip wfu.orig
printf '%s\n' 'zip I/O error: File too large' \
    'zip error: Output file write failure (write error on zip file)' >want
for arc in w u; do
    (trap '' XFSZ; ulimit -f 2000; exec "$ZIP" wf/$arc.zip wf/big wf/small) \
        >out 2>err && fail "failed write succeeded ($arc)"
    grep -qx '  adding: wf/big' out || fail "failed write ($arc): $(cat out)"
    cmp -s err want || fail "failed write ($arc): $(cat err)"
done
cmp -s wf/u.zip wfu.orig || fail "failed write changed the archive"
[ "$(ls wf | tr '\n' ' ')" = "big small u.zip " ] ||
    fail "failed write left: $(ls wf)"

# With nothing to update, -u and -f exit 12 silently, as Info-ZIP does,
# and on a missing archive it warns
"$ZIP" -u u.zip missing >out 2>&1 && fail "-u with nothing found"
grep -q 'Nothing to do' out && fail "-u with nothing found: $(cat out)"
"$ZIP" -f u0.zip missing >out 2>&1 && fail "-f with nothing found"
grep -q 'Nothing to do' out && fail "-f with nothing found: $(cat out)"
"$ZIP" -u newu.zip tree/a.txt >out 2>&1
grep -q 'newu.zip not found or empty' out || fail "-u, no archive: $(cat out)"
names newu.zip | grep -q tree/a.txt || fail "-u, no archive: $(names newu.zip)"

# -d and -f warn the same of a missing or empty archive, then go on
# with their arguments, as Info-ZIP does
"$ZIP" -d gone.zip nosuch tree/a.txt >out 2>&1 && fail "-d, no archive"
grep -q 'gone.zip not found or empty' out || fail "-d, no archive: $(cat out)"
grep -q 'name not matched: nosuch' out || fail "-d, no archive: $(cat out)"
grep -q 'name not matched: tree/a.txt' out && fail "-d, no archive: $(cat out)"
grep -q 'Nothing to do' out || fail "-d, no archive: $(cat out)"
expect_status 16 "$ZIP" -f gone.zip tree/a.txt ./tree/a.txt
"$ZIP" -f gone.zip tree/a.txt nosuch >out 2>&1 && fail "-f, no archive"
grep -q 'name not matched: nosuch' out || fail "-f, no archive: $(cat out)"
"$ZIP" -q em.zip tree/a.txt
"$ZIP" -qd em.zip tree/a.txt
expect_status 12 "$ZIP" -f em.zip tree/b.txt
for mode in -d -f -u; do  # the last adds
    "$ZIP" $mode em.zip tree/b.txt >out 2>&1 || true
    grep -q 'em.zip not found or empty' out || fail "$mode, empty: $(cat out)"
done
[ ! -e gone.zip ] || fail "-d or -f created an archive"

if [ "$(id -u)" != 0 ]; then
    cp tree/a.txt unreadable
    chmod 000 unreadable
    expect_status 18 "$ZIP" r.zip tree/b.txt unreadable
    names r.zip >got
    printf 'tree/b.txt\n' >want
    cmp -s got want || fail "unreadable file: $(cat got)"

    # As in Info-ZIP, the progress line comes first, then the system's
    # reason, and the warning gives the entry's name. Its summary counts
    # what was read, and abbreviates large byte counts.
    "$ZIP" -j r2.zip ./unreadable tree/b.txt >out 2>err &&
        fail "unreadable file succeeded"
    grep -qx '  adding: unreadable' out || fail "unreadable: $(cat out)"
    cat >want <<EOF
zip warning: Permission denied
zip warning: could not open for reading: unreadable

zip warning: Not all files were readable
  files/entries read:  1 (1 bytes)  skipped:  1 (24 bytes)
EOF
    cmp -s err want || fail "unreadable: $(cat err)"
    "$ZIP" -j r3.zip unreadable tree/sub/random >out 2>err || true
    grep -qx '  files/entries read:  1 (292K bytes)  skipped:  1 (24 bytes)' \
        err || fail "unreadable, large: $(cat err)"
    # Info-ZIP's perror lines come even under -q
    "$ZIP" -q r4.zip unreadable 2>err && fail "unreadable file, -q"
    [ "$(cat err)" = 'zip warning: Permission denied' ] ||
        fail "-q, unreadable: $(cat err)"

    # As there, the summary comes before "zip file empty"
    "$ZIP" r5.zip unreadable >out 2>err && fail "unreadable file alone"
    cat >want <<EOF
zip warning: Permission denied
zip warning: could not open for reading: unreadable

zip warning: Not all files were readable
  files/entries read:  0 (0 bytes)  skipped:  1 (24 bytes)
zip warning: zip file empty
EOF
    cmp -s err want || fail "unreadable file alone: $(cat err)"
    chmod 644 unreadable

    # Failing to create the temporary file is Info-ZIP's temporary file
    # failure (10) when replacing an archive, else it names the archive
    mkdir ro
    "$ZIP" -q ro/x.zip tree/a.txt
    cp ro/x.zip rodir.orig
    chmod 555 ro
    expect_status 10 "$ZIP" ro/x.zip tree/b.txt
    "$ZIP" ro/new.zip tree/b.txt 2>err && fail "archive in read-only directory"
    printf '%s\n' 'zip I/O error: Permission denied' \
        'zip error: Could not create output file (ro/new.zip)' >want
    cmp -s err want || fail "read-only directory: $(cat err)"
    # Departure: so does a link to it from a directory that allows them,
    # as the archive is replaced where links lead (Info-ZIP copies into
    # it, its temporary file beside the link)
    if ln -s ro/x.zip rol.zip 2>/dev/null; then
        expect_status 10 "$ZIP" rol.zip tree/b.txt
    fi
    cmp -s ro/x.zip rodir.orig || fail "archive in read-only directory changed"
    chmod 755 ro

    # A read-only archive is refused (15) and left alone, as Info-ZIP
    # finds, after reading it and finding something to do, before doing
    # it. One that can be neither read nor written is, as there, taken
    # for a missing archive, which cannot be written (15) either.
    "$ZIP" -q rox.zip tree/a.txt
    cp rox.zip rox.orig
    chmod 444 rox.zip
    "$ZIP" rox.zip tree/b.txt >out 2>err && fail "read-only archive updated"
    printf '%s\n' 'zip I/O error: Permission denied' \
        'zip error: Could not create output file (rox.zip)' >want
    cmp -s err want || fail "read-only archive: $(cat err)"
    grep -q adding out && fail "read-only archive: work done first"
    expect_status 15 "$ZIP" -d rox.zip tree/a.txt
    expect_status 15 "$ZIP" -FS rox.zip tree/b.txt
    expect_status 12 "$ZIP" rox.zip missing
    cmp -s rox.zip rox.orig || fail "read-only archive changed"
    ln -s rox.zip roxl.zip && expect_status 15 "$ZIP" roxl.zip tree/b.txt
    chmod 000 rox.zip
    expect_status 15 "$ZIP" rox.zip tree/b.txt
    expect_status 15 "$ZIP" -u rox.zip tree/b.txt
    expect_status 12 "$ZIP" -f rox.zip tree/b.txt
    "$ZIP" -d rox.zip tree/a.txt >out 2>&1 && fail "-d on mode 000 archive"
    grep -q 'rox.zip not found or empty' out || fail "-d, 000: $(cat out)"
    grep -q 'Nothing to do' out || fail "-d, 000: $(cat out)"

    # Departure: one that can be written but not read fails (11), where
    # Info-ZIP takes it for a missing archive and replaces it
    chmod 200 rox.zip
    expect_status 11 "$ZIP" rox.zip tree/b.txt
    chmod 644 rox.zip
    cmp -s rox.zip rox.orig || fail "unreadable archive changed"
    echo junk >roj.zip
    chmod 444 roj.zip
    expect_status 3 "$ZIP" roj.zip tree/b.txt

    # Departure: an unreadable directory while recursing exits 18, where
    # Info-ZIP adds it silently (0)
    mkdir -p ud/sub
    chmod 000 ud/sub
    expect_status 18 "$ZIP" -r ud.zip ud
    [ "$(names ud.zip | tr '\n' ' ')" = "ud/ ud/sub/ " ] ||
        fail "unreadable directory: $(names ud.zip)"
    # ...unless -x or -i leaves out all it could hold, silently: -x of a
    # start of its name and then only stars, or -i whose patterns begin
    # otherwise; but not -x of its own entry, nor -i that may match
    printf x >ud/f
    "$ZIP" -qr ud2.zip ud -x 'ud/sub/*' 2>err ||
        fail "excluded unreadable directory: $(cat err)"
    [ ! -s err ] || fail "excluded unreadable directory: $(cat err)"
    [ "$(names ud2.zip | tr '\n' ' ')" = "ud/ ud/f " ] ||
        fail "excluded unreadable directory: $(names ud2.zip)"
    expect_status 0 "$ZIP" -r ud3.zip ud -x 'ud/su*'
    expect_status 0 "$ZIP" -r ud4.zip ud -i 'ud/f' 'src/*'
    expect_status 18 "$ZIP" -r ud7.zip ud -i 'ud/f' '*.txt'
    expect_status 0 "$ZIP" -r ud8.zip ud -i 'ud/f' ''  # matching nothing
    expect_status 18 "$ZIP" -r ud5.zip ud -x 'ud/sub/'
    expect_status 18 "$ZIP" -r ud6.zip ud -i 'ud/*'
    chmod 755 ud/sub

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
    "$ZIP" s.zip stale >out 2>err && fail "unreadable replacement succeeded"
    cat >want <<EOF
stale: Permission denied
zip warning: could not open for reading: stale
zip warning: will just copy entry over: stale
EOF
    cmp -s err want || fail "copy over warning: $(cat err)"
    "$ZIP" -q s.zip stale >out 2>err && fail "unreadable replacement, -q"
    [ "$(cat err)" = 'stale: Permission denied' ] ||
        fail "copy over, -q: $(cat err)"
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
# ...where, as in Info-ZIP on POSIX, a backslash is dropped, keeping the
# byte after it, even a quote or a backslash (and a final one escapes
# nothing), while unquoted words are kept whole
env ZIPOPT='"e\"n\\v\6" -q' "$ZIP" tree/a.txt
[ -f 'e"n\v6.zip' ] || fail "ZIPOPT quoted backslashes: $(ls | grep '^e')"
env ZIPOPT='-q e\"n\v7' "$ZIP" tree/a.txt
[ -f 'e\"n\v7.zip' ] || fail "ZIPOPT unquoted backslashes: $(ls | grep '^e')"
env ZIPOPT='-q "env8\' "$ZIP" tree/a.txt
[ -f env8.zip ] || fail "ZIPOPT final backslash: $(ls | grep '^env')"
expect_status 16 env ZIPOPT=-e "$ZIP" env5.zip tree/a.txt
env ZIPOPT=-v "$ZIP" </dev/null >out
grep -q '^tugz zip [0-9]' out || fail "ZIPOPT=-v: $(cat out)"

# Options -d ignores, as Info-ZIP warns
"$ZIP" -q dw.zip tree/a.txt tree/b.txt
"$ZIP" -d -r dw.zip tree/a.txt >out 2>&1
grep -q 'invalid option(s) used with -d; ignored' out ||
    fail "-d -r: $(cat out)"
[ "$(names dw.zip)" = tree/b.txt ] || fail "-d -r: $(names dw.zip)"

# A new archive gets the permissions of a new file, here one the shell
# creates (a default ACL may decide them, not the umask), while a
# replaced one keeps its own
for mask in 022 077; do
    rm -f perm.zip perm.ref
    (umask $mask && : >perm.ref && "$ZIP" -q perm.zip tree/a.txt)
    [ "$(ls -l perm.zip | cut -c1-10)" = "$(ls -l perm.ref | cut -c1-10)" ] ||
        fail "new archive mode, umask $mask: $(ls -l perm.zip perm.ref)"
done
chmod 640 perm.zip
(umask 022 && "$ZIP" -q perm.zip tree/b.txt)
[ "$(ls -l perm.zip | cut -c1-10)" = "-rw-r-----" ] || fail "replaced mode"
# Its set-UID and sticky bits too, as in Info-ZIP, with its owner and
# group unchanged (set-GID may not be the user's to set)
for m in 4755 1755; do
    chmod $m perm.zip 2>/dev/null || continue  # BSD: no sticky files
    want=$(ls -l perm.zip | cut -c1-10)
    "$ZIP" -q perm.zip tree/a.txt
    [ "$(ls -l perm.zip | cut -c1-10)" = "$want" ] ||
        fail "replaced mode $m: $(ls -l perm.zip)"
done
chmod 644 perm.zip
# Its group too, where the user may give it (Info-ZIP's rename keeps
# none), as when a supplementary group shares it. Else the new file's
# group (the user's, or on BSD the directory's) gets only the permissions
# of others, not the old group's: here a group the user is not in, which
# BSD gave the archive from its directory, before that changed.
gid() { ls -ln "$1" | awk '{print $4}'; }
mkdir grp
: >grp/probe
newgid=$(gid grp/probe)
member=
other=
for g in $(id -G); do
    [ $g = $newgid ] && member=1
    [ $g != $newgid ] && [ -z "$other" ] && other=$g
done
"$ZIP" -q grp/g.zip tree/a.txt
if [ -n "$other" ] && chgrp $other grp/g.zip 2>/dev/null; then
    chmod 660 grp/g.zip
    "$ZIP" -q grp/g.zip tree/b.txt
    [ "$(gid grp/g.zip)" = $other ] &&
        [ "$(ls -l grp/g.zip | cut -c1-10)" = -rw-rw---- ] ||
        fail "replaced group: $(ls -ln grp/g.zip)"
fi
if [ -z "$member" ] && [ "$(id -u)" != 0 ]; then
    "$ZIP" -q grp/n.zip tree/a.txt
    chmod 654 grp/n.zip
    chgrp "$(id -g)" grp
    "$ZIP" -q grp/n.zip tree/b.txt
    [ "$(gid grp/n.zip)" = "$(id -g)" ] &&
        [ "$(ls -l grp/n.zip | cut -c1-10)" = -rw-r--r-- ] ||
        fail "replaced group not kept: $(ls -ln grp/n.zip)"
fi

# With standard output closed, progress lines stay out of the archive
"$ZIP" -r closed.zip tree >&-
verify closed.zip

# Without /dev/null to fill the closed descriptor (a sandbox), zip exits
# before creating anything rather than let the archive take its place
nonull() {
    if command -v bwrap >/dev/null; then
        bwrap --bind / / --tmpfs /dev "$@"
    else
        sandbox-exec -p '(version 1) (allow default)
            (deny file-read* file-write* (literal "/dev/null"))' "$@"
    fi
}
if nonull true 2>/dev/null && ! nonull sh -c ': </dev/null' 2>/dev/null
then
    set +e
    nonull sh -c 'exec "$0" -r nonull.zip tree >&-' "$ZIP" 2>err
    st=$?
    set -e
    [ "$st" = 10 ] || fail "status $st without /dev/null"
    grep -q "Could not open /dev/null" err || fail "no /dev/null message"
    [ ! -e nonull.zip ] || fail "archive created without /dev/null"
fi

# A reader that goes away (SIGPIPE) leaves no temporary file behind
mkdir pipe
(cd pipe && "$ZIP" -r out.zip ../tree | true)
case "$(ls pipe)" in zi*) fail "temporary file left on SIGPIPE";; esac

# Nor does one terminated while writing, which leaves the archive as it
# was. (When zip ends too soon to be stopped, there is nothing to check:
# with 2 MB to write, rarely, and then only under heavy load.)
mkdir race rz
head -c 2000000 /dev/urandom >race/a_big
"$ZIP" -q rz/race.zip tree/a.txt
cp rz/race.zip race.orig
if bgzip rz -q rz/race.zip race/a_big; then
    kill -TERM $pid
    kill -CONT $pid
    wait $bg
    [ "$(cat bg.status)" = 143 ] || fail "not terminated: $(cat bg.status)"
    cmp -s rz/race.zip race.orig || fail "a terminated run changed the archive"
    [ "$(ls rz)" = race.zip ] || fail "terminated run left: $(ls rz)"
else
    wait $bg
    echo "zip.sh: zip finished before it could be terminated" >&2
fi

# Departure: a new archive replaces nothing, so that one made at its path
# meanwhile is kept, and zip fails to replace it (15) rather than lose
# what it never read (Info-ZIP replaces it)
rm -f rz/new.zip
if bgzip rz -q rz/new.zip race/a_big; then
    "$ZIP" -q rz/new.zip tree/a.txt
    kill -CONT $pid
    wait $bg
    w='Could not create output file (was replacing the original zip file)'
    [ "$(cat bg.status)" = 15 ] && [ "$(names rz/new.zip)" = tree/a.txt ] &&
        grep -qF "$w" bg.err ||
        fail "archive made meanwhile: $(cat bg.status) $(names rz/new.zip)" \
             "$(cat bg.err)"
else
    wait $bg
    echo "zip.sh: zip finished before an archive could be made meanwhile" >&2
fi

# An archive that cannot be replaced, once its temporary file is written,
# fails as in Info-ZIP (15), and is left as it was. Here its directory
# refuses the rename, and also removing the temporary file, which is
# then left, as Info-ZIP leaves it, with its warning.
cp race.orig rz/race.zip
if [ "$(id -u)" != 0 ] && bgzip rz rz/race.zip race/a_big; then
    chmod 555 rz
    kill -CONT $pid
    wait $bg
    chmod 755 rz
    left=$(cd rz && echo zi[0-9]*)
    cat >want <<EOF
zip warning: new zip file left as: rz/$left
zip I/O error: Permission denied
zip error: Could not create output file (was replacing the original zip file)
EOF
    [ "$(cat bg.status)" = 15 ] && cmp -s bg.err want ||
        fail "failed replacement: $(cat bg.status) $(cat bg.err)"
    cmp -s rz/race.zip race.orig || fail "failed replacement changed it"
    rm rz/zi[0-9]*
elif [ "$(id -u)" != 0 ]; then
    wait $bg
    echo "zip.sh: zip finished before its replacement could fail" >&2
fi

# A file swapped since the scan is not read: under -y, for a link, or a
# file through a link swapped in for its directory, and in any case for
# a FIFO (which a zip opening it would wait on, but here has a writer,
# so that it goes on to read). Zip is stopped while it writes a large
# file, to swap the next one. If zip read it too soon, nothing is
# checked.
swapped() {  # name link|fifo|dir zip-option...
    name=$1
    kind=$2
    shift 2
    rm -f rz/race.zip
    file=race/$name
    want=race/a_big
    if [ $kind = dir ]; then
        mkdir race/$name
        file=race/$name/f
        want="race/a_big race/$name/"
    fi
    printf small >$file
    if ! bgzip rz "$@" rz/race.zip race/a_big race/$name; then
        wait $bg
        echo "zip.sh: zip finished before race/$name could be swapped" >&2
        return
    fi
    if [ $kind = dir ]; then
        mv race/$name race/$name.old
        ln -s ../other race/$name
    elif [ $kind = link ]; then
        rm race/$name
        ln -s ../secret race/$name
    else
        rm race/$name
        mkfifo race/$name
        printf leaked >race/$name &
        writer=$!
    fi
    kill -CONT $pid
    wait $bg
    if [ $kind = fifo ]; then
        exec 3<>race/$name 3<&-  # end the writer if zip never opened it
        wait $writer 2>/dev/null || :  # (SIGPIPE if zip closed it first)
    fi
    if [ "$(unzip -p rz/race.zip $file 2>/dev/null)" = small ]; then
        echo "zip.sh: zip read $file before it could be swapped" >&2
        return
    fi
    [ "$(cat bg.status)" = 18 ] &&
        [ "$(names rz/race.zip | tr '\n' ' ')" = "$want " ] &&
        grep -q "could not open for reading: $file" bg.err ||
        fail "$kind swapped in: $(cat bg.status) $(names rz/race.zip)" \
             "$(cat bg.err)"
}
if ln -s x race/link 2>/dev/null && mkfifo race/fifo 2>/dev/null; then
    rm race/link race/fifo
    printf 'TOP SECRET' >secret
    mkdir other
    printf 'TOP SECRET' >other/f
    swapped b_link link -y
    swapped c_fifo fifo
    swapped d_dir dir -ry
fi

# A file whose size has changed since the scan, which gave the entry its
# time, is stored as read, with the warning of Info-ZIP's Unix port, for
# one that changes while it reads it, which leaves the status alone
rm -f rz/race.zip
printf small >race/e_grow
if bgzip rz rz/race.zip race/a_big race/e_grow; then
    printf er >>race/e_grow
    kill -CONT $pid
    wait $bg
    if [ "$(unzip -p rz/race.zip race/e_grow)" = small ]; then
        echo "zip.sh: zip read race/e_grow before it could grow" >&2
    else
        w='zip warning:  file size changed while zipping race/e_grow'
        [ "$(cat bg.status)" = 0 ] && [ "$(cat bg.err)" = "$w" ] &&
            [ "$(unzip -p rz/race.zip race/e_grow)" = smaller ] ||
            fail "file grown: $(cat bg.status) $(cat bg.err)"
    fi
else
    wait $bg
    echo "zip.sh: zip finished before race/e_grow could grow" >&2
fi

# Running out of memory exits 4, as in Info-ZIP, before any output, as
# zip allocates what grows with its work first: no archive is created
# (here under one limit) or changed (under the other), and no temporary
# file is left. Linux limits address space (ulimit -v), into which zip's
# reservation then shrinks, and private writable memory (ulimit -d),
# against which each commit counts. A small run fits under each, while
# the 65,535 paths through a chain of directories, each linking twice to
# the next, need some 250 MB. A file that only ends like an archive,
# whose directory of zeros (sparse, 92 MB) would hold 2M entries, is not
# one (3), however much memory those would take. macOS ignores these
# limits, and sanitizers and emulators cannot run under them.
oomskip=
if [ "$(uname -s)" != Linux ]; then
    oomskip="not Linux"
elif LC_ALL=C grep -aq -e __asan_ -e __hwasan_ -e __lsan_ -e __msan_ \
                       -e __tsan_ -e __ubsan_ -e liblsan "$ZIP"; then
    oomskip="sanitized"
else
    mkdir oom oomdag
    long=$(awk 'BEGIN{while(length(s)<250)s=s "x";print s}')
    for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14; do
        mkdir oomdag/$i
        ln -s ../$((i+1)) oomdag/$i/a$long
        ln -s ../$((i+1)) oomdag/$i/b$long
    done
    mkdir oomdag/15
    printf '\nzip error: Out of memory\n' >want
    if [ -n "$PY" ]; then
        $PY -c 'import struct, sys
n = 2000000
f = open(sys.argv[1], "wb")
f.truncate(46*n)
f.seek(46*n)
f.write(struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 0x31e, 45, 0, 0, n, n,
                    46*n, 0))
f.write(struct.pack("<IIQI", 0x07064b50, 0, 46*n, 1))
f.write(struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 0xffff, 0xffff,
                    0xffffffff, 0xffffffff, 0))' oomsparse.zip
    fi
fi
for run in "-v 60000 new.zip" "-d 30000 small.zip"; do  # limit, archive
    limit=${run% *}
    arc=${run##* }
    [ -z "$oomskip" ] || break
    set +e
    (ulimit $limit && exec "$ZIP" -v) >out 2>err
    st=$?
    set -e
    if [ $st != 0 ] && [ $st != 4 ] && [ $st -lt 128 ]; then
        oomskip="no start under ulimit $limit: $(head -n 1 err)"
        break
    fi
    grep -q '^tugz zip' out || fail "zip -v under ulimit $limit: $st $(cat err)"
    rm -f oom/*
    (ulimit $limit && exec "$ZIP" -qr oom/small.zip tree) ||
        fail "a small run under ulimit $limit"
    extract_same oom/small.zip tree
    cp oom/small.zip oom.orig
    set +e
    (ulimit $limit && exec "$ZIP" -qr oom/$arc oomdag/0) 2>err
    st=$?
    set -e
    [ $st = 4 ] && cmp -s err want ||
        fail "ulimit $limit, $arc: status $st $(cat err)"
    cmp -s oom/small.zip oom.orig || fail "ulimit $limit changed an archive"
    [ "$(ls oom)" = small.zip ] || fail "ulimit $limit left: $(ls oom)"
    if [ -e oomsparse.zip ]; then
        set +e
        (ulimit $limit && exec "$ZIP" -q oomsparse.zip tree/a.txt) 2>err
        st=$?
        set -e
        [ $st = 3 ] && grep -q 'structure invalid (oomsparse.zip)' err ||
            fail "ulimit $limit, sparse: status $st $(cat err)"
    fi
done
[ -z "$oomskip" ] || echo "zip.sh: out-of-memory tests skipped: $oomskip" >&2
rm -rf oomdag oomsparse.zip

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

    # Zip64 for a file of exactly 4 GiB - 1 bytes, after which an entry
    # past 4 GiB stays readable by UnZip 6.0, as every central Zip64 extra
    # holds both sizes
    dd if=/dev/zero of=ff bs=1 count=0 seek=4294967295 2>/dev/null
    "$ZIP" -q0 ff0.zip ff tree/a.txt
    verify ff0.zip
    [ "$(unzip -p ff0.zip tree/a.txt)" = "hello hello hello hello" ] ||
        fail "entry past 4 GiB after 4 GiB - 1"
    rm ff0.zip

    # Info-ZIP writes that size literally, without Zip64, and merging into
    # such an archive works (made here by removing the Zip64 fields)
    "$ZIP" -q1 ff1.zip ff
    if [ -n "$PY" ]; then
        $PY -c 'import struct, sys
d = open(sys.argv[1], "rb").read()
def strip(x):  # extra fields but Zip64
    r = b""
    while len(x) >= 4:
        i, n = struct.unpack("<HH", x[:4])
        r += x[:4+n] if i != 1 else b""
        x = x[4+n:]
    return r
h = struct.unpack("<IHHHHHIIIHH", d[:30])
p = d.rfind(b"PK\x01\x02")
c = struct.unpack("<IHHHHHHIIIHHHHHII", d[p:p+46])
name = d[30:30+h[9]]
data = d[30+h[9]+h[10]:p]
lx = strip(d[30+h[9]:30+h[9]+h[10]])
cx = strip(d[p+46+c[10]:p+46+c[10]+c[11]])
loc = struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, h[2], h[3], h[4], h[5],
                  h[6], len(data), 0xffffffff, len(name), len(lx))
cen = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, c[1], 20, c[3], c[4],
                  c[5], c[6], c[7], len(data), 0xffffffff, len(name),
                  len(cx), 0, 0, c[14], c[15], 0)
loc += name + lx
cen += name + cx
end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(cen),
                  len(loc) + len(data), 0)
open(sys.argv[1], "wb").write(loc + data + cen + end)' ff1.zip
        verify ff1.zip
        "$ZIP" -q ff1.zip tree/b.txt
        verify ff1.zip
        [ "$(names ff1.zip | tr '\n' ' ')" = "ff tree/b.txt " ] ||
            fail "merge after a literal 4 GiB - 1: $(names ff1.zip)"
        "$ZIP" -qd ff1.zip ff
        [ "$(names ff1.zip)" = tree/b.txt ] || fail "-d of a literal 4 GiB - 1"
    fi
    rm ff ff1.zip

    # Zip64: more than 65,535 entries
    mkdir many
    (cd many && awk 'BEGIN{for(i=0;i<70000;i++)print i}' | xargs touch)
    "$ZIP" -qr many.zip many
    verify many.zip
    [ "$(names many.zip | wc -l | tr -d ' ')" = 70001 ] || fail "70,000 entries"
    "$ZIP" -q many.zip tree/a.txt
    verify many.zip
    [ "$(names many.zip | wc -l | tr -d ' ')" = 70002 ] || fail "merge many"
    if [ -n "$PY" ]; then  # versions made by and needed, as in Info-ZIP
        $PY -c 'import sys
d = open(sys.argv[1], "rb").read()
p = d.rfind(b"PK\x06\x06")
sys.exit(d[p+12:p+16] != b"\x1e\x03\x2d\x00")' many.zip ||
            fail "Zip64 end record versions"
    fi
fi

# No run left a temporary file
find . -name 'zi[0-9][0-9][0-9][0-9][0-9][0-9]' >out
[ ! -s out ] || fail "temporary files left: $(cat out)"

echo "zip tests pass"
