#!/bin/sh
# End-to-end tests of an unzip binary: listing, testing (-l, -v, -t, -p,
# -c, -z), and extracting, comparing its status, standard output, and
# standard error, and the trees it extracts, with Info-ZIP's UnZip, REF,
# on archives from Python (test/unzipcraft.py, via uv when available),
# printf, tugz's zip (TUGZ_ZIP), and Info-ZIP's zip if present. Where
# tugz departs from UnZip, as documented, its output is checked on its
# own, as is what it must never do: write outside the destination.
# Usage: REF=/usr/bin/unzip sh test/unzip.sh ./unzip
# Set UNZIPOOM to another build for the out-of-memory tests, as ctest
# does, its own build being sanitized.
# Set SLOW=1 to include Zip64 tests: entries of 4 GiB and more, which
# need about 13 GiB free in TMPDIR. Archives over 4 GiB are otherwise
# sparse, and skipped where TMPDIR's file system has no holes.
set -e

unset UNZIP UNZIPOPT ZIPOPT ZIP  # options from the environment
export TZ=UTC LC_ALL=C           # listings' times, and REF's names
U=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
if [ -n "$UNZIPOOM" ]; then  # another build for the out-of-memory tests
    UNZIPOOM=$(cd "$(dirname "$UNZIPOOM")" && pwd)/$(basename "$UNZIPOOM")
fi
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
trap 'cd / && chmod -R u+rwx "$tmp" && rm -rf "$tmp"' EXIT
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

# A listing of an extracted tree: each path's type and permissions, a
# file's content, a link's target, and modification times, of files and
# directories, of files alone (TIMES=files: directories that no entry
# made, or that -D leaves alone, have the time they were made), or of
# none (TIMES=none)
if stat -c %a . >/dev/null 2>&1; then
    perms() { stat -c %a "$1"; }
    mtime() { stat -c %Y "$1"; }
else
    perms() { stat -f %Lp "$1"; }
    mtime() { stat -f %m "$1"; }
fi
tree() {
    (cd "$1" && find . ! -name . | LC_ALL=C sort | while IFS= read -r f; do
        t=
        if [ -L "$f" ]; then
            echo "L $f -> $(readlink "$f")"
        elif [ -d "$f" ]; then
            [ "${TIMES:-all}" != all ] || t=$(mtime "$f")
            echo "D $f $(perms "$f") $t"
        else
            [ "${TIMES:-all}" = none ] || t=$(mtime "$f")
            echo "F $f $(perms "$f") $t $(cksum <"$f" 2>/dev/null || true)"
        fi
    done)
}

# Extract with ours and REF on the same arguments, each in a directory of
# its own, x.ours and x.ref, made anew and set up by the shell commands
# $SETUP, with standard input from $IN, leaving their status, output,
# errors, and trees in ours.* and ref.*. Apple's REF names a few paths in
# full, and leaves the '/' off a directory it creates: those are made as
# other builds have them.
xboth() {
    for d in x.ours x.ref; do
        [ ! -e $d ] || chmod -R u+rwx $d
        rm -rf $d
        mkdir $d
        [ -z "$SETUP" ] || (cd $d && eval "$SETUP") || fail "setup: $SETUP"
    done
    set +e
    (cd x.ours && exec "$U" "$@") <"${IN:-/dev/null}" >ours.out 2>ours.err
    echo $? >ours.st
    (cd x.ref && exec "$REF" "$@") <"${IN:-/dev/null}" >ref.out 2>ref.err
    echo $? >ref.st
    set -e
    [ "$(cat ours.st)" != 99 ] || fail "sanitizer: $*: $(cat ours.err)"
    if [ $apple = 1 ]; then
        for f in out err; do
            sed -e "s|$(cd x.ref && pwd -P)/||g" -e "s|$(cd x.ref && pwd)/||g" \
                -e 's|^   creating: \(.*[^/]\)$|   creating: \1/|' \
                ref.$f >ref.tmp
            mv ref.tmp ref.$f
        done
    fi
    tree x.ours >ours.tree
    tree x.ref >ref.tree
}

# The same status, output, errors, and tree as REF's.
xsame() {
    xboth "$@"
    cmp -s ours.st ref.st ||
        fail "status $(cat ours.st), REF's $(cat ref.st): $*: $(cat ours.err)"
    cmp -s ours.out ref.out ||
        fail "output differs from REF's: $*: $(diff ref.out ours.out)"
    cmp -s ours.err ref.err ||
        fail "errors differ from REF's: $*: $(diff ref.err ours.err)"
    cmp -s ours.tree ref.tree ||
        fail "tree differs from REF's: $*: $(diff ref.tree ours.tree)"
}

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
    xsame ../z.zip                            # extracted
    # tree/ is made, not extracted, so has the time it was made
    TIMES=files
    xsame -q ../z.zip 'tree/*.txt' -x tree/b.txt
    TIMES=all
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
# Archives over 4 GiB only where files may have holes: a 1 GiB hole
# that takes (nearly) no room
holes=
if dd if=/dev/null of=holes bs=1048576 seek=1024 count=0 2>/dev/null &&
   [ "$(du -k holes | cut -f1)" -lt 1024 ]; then
    holes=sparse
else
    echo "unzip.sh: skipping archives over 4 GiB: no sparse files here" >&2
fi
rm -f holes
mkdir craft
(cd craft && $PY "$CRAFT" $holes) || fail "unzipcraft.py"
cd craft
: >none

# Archives that tugz reads as UnZip does, in every mode, among them end
# records that UnZip reads past: counts other than the headers there, a
# count wrapped past 65,535, Zip64 records after unadjusted data, and
# comments that the file cuts short
for z in basic nocomment sfx sfxok zip64 desc empty emptycomment emptysfx \
         multi many badcrc baddata truncdata storedsize localname badextra \
         truncated badcdoff badheader badlocal deflate64 \
         count2 count4 count5 count0 count64 sfx64 sfx64ok nosig64 loc64 \
         cmtcut cmtnone cmtcut64 cdjunk cmtlines; do
    for mode in -l -v -t -tq -tqq -p -pq -z -zq -c -cq; do
        [ $apple = 1 ] && [ "$mode" = -c ] && continue  # Apple's names
        same $mode $z.zip
    done
done
same -l wrap.zip                              # 65,539 entries, a count of 3
same -t wrap.zip
same -l basic                                 # with .zip
same -t basic dir/text.txt
same -l methods.zip
same -v methods.zip
same -t overrun.zip

# Departure: -v's compression factor where UnZip's arithmetic overflows,
# for a Zip64 size and an encrypted entry's compressed size under 12,
# wrapped: the growth, as large as fits, where Debian's UnZip shows
# " 214748364%" (a sanitized build traps on any overflow)
exits 0 "$U" -v ratio.zip >got 2>&1
grep -q ' -214748364% ' got || fail "-v ratio.zip: $(cat got)"

# Departure: a Deflate64 match from before the entry's start is invalid
# data, as a deflated one is (zlib's check), where UnZip's own inflate
# copies from its window as it was, then finds a bad CRC
{ printf 'Archive:  deflate64bad.zip\n    testing: %-22s  \n' bad.bin
  printf '  error:  invalid compressed data to inflate\n'
  printf '    testing: %-22s   OK\n' short.bin
  printf 'At least one error was detected in deflate64bad.zip.\n'; } >want
ours 2 want none -t deflate64bad.zip

# Departure: an entry is read by its central header's CRC and sizes, not
# its local header's, which here differ: UnZip finds a bad CRC in the
# first entry and extracts three bytes of the second
{ printf 'Archive:  localsizes.zip\n'
  printf '    testing: %-22s   OK\n' a.txt s.txt
  printf 'No errors detected in compressed data of localsizes.zip.\n'; } >want
ours 0 want none -t localsizes.zip
"$U" -p basic.zip dir/text.txt dir/stored.txt >want
ours 0 want none -p localsizes.zip
both -t localsizes.zip
[ "$(cat ref.st)" = 2 ] || fail "localsizes.zip: REF's status $(cat ref.st)"

# Departure: a Zip64 end record is used beside an end record, not
# saturated, that counts otherwise, where UnZip goes by the end record:
# a Zip64 count of 1 of 3 headers is the count fault that UnZip finds
# when a saturated end record defers to it (count64.zip), and one of 3
# beside an end record's 4 is read without error
{ printf 'Archive:  mixed64.zip\n'
  printf 'error:  expected central file header signature not found '
  printf '(file #4).\n'
  printf '  (please check that you have transferred or created the zipfile '
  printf 'in the\n  appropriate BINARY mode and that you have compiled '
  printf 'UnZip properly)\n'
  printf '    testing: %-22s   OK\n' a b c
  printf 'At least one error was detected in mixed64.zip.\n'; } >want
ours 3 want none -t mixed64.zip
{ printf 'Archive:  mixed64ok.zip\n'
  printf '    testing: %-22s   OK\n' a b c
  printf 'No errors detected in compressed data of mixed64ok.zip.\n'; } >want
ours 0 want none -t mixed64ok.zip
both -t mixed64.zip
[ "$(cat ref.st)" = 2 ] || fail "mixed64.zip: REF's status $(cat ref.st)"
both -t mixed64ok.zip
[ "$(cat ref.st)" = 3 ] || fail "mixed64ok.zip: REF's status $(cat ref.st)"

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
# encrypted, and others (UnZip builds that have them, as REF, decode
# them), but for Deflate64, which tugz decodes too
cat >want <<EOF
Archive:  methods.zip
   skipping: enc.txt                 encrypted (not supported)
   skipping: bzip2.bin               \`bzip2' method not supported
   skipping: lzma.bin                \`LZMA' method not supported
   skipping: shrunk.bin              \`shrink' method not supported
   skipping: aes.bin                 unsupported compression method 99
   skipping: new.bin                 need PK compat. v6.3 (can do v4.5)
    testing: ok.txt                   OK
    testing: d64.bin                  OK
    testing: last.txt                 OK
No errors detected in methods.zip for the 3 files tested.
6 files skipped because of unsupported compression or encoding.
EOF
ours 81 want none -t methods.zip
grep '^   skipping' want >want.err
"$REF" -p methods.zip ok.txt d64.bin last.txt >want.out
ours 81 want.out none -p methods.zip           # -p says nothing
"$U" -c methods.zip ok.txt d64.bin last.txt >want.out
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

# Overlapped components, a zip bomb's: found before any entry is read
# (Debian's UnZip finds them as it reads the second), with Debian's
# message and status; data reaching into the central directory too
# (which Debian's does not check), even past the end of the file, by a
# central compressed size that UnZip lists, then reads by the local one
for z in overlap inner overlapcd csizepast; do
    printf 'Archive:  %s.zip\nerror: invalid zip file with overlapped components (possible zip bomb)\n' $z >want
    ours 12 want none -t $z.zip
    printf 'error: invalid zip file with overlapped components (possible zip bomb)\n' >want
    ours 12 want none -tq $z.zip
    ours 12 none want -p $z.zip
    if [ $debian = 1 ] && [ $z != overlapcd ] && [ $z != csizepast ]; then
        same -tq $z.zip
        same -pq $z.zip 'b*'
    fi
    same -l $z.zip                            # listing reads no data
    same -z $z.zip
done
# Its other entries, selected alone, are read
same -t csizepast.zip a.txt c.txt
# and -v as UnZip's, but for the departure in its compression factor,
# where UnZip's arithmetic overflows (above)
both -v csizepast.zip
sed 's/ -214748365% / -214748364% /' ref.out >ref.v
cmp -s ours.st ref.st && cmp -s ours.out ref.v && cmp -s ours.err ref.err ||
    fail "-v csizepast.zip: $(diff ref.v ours.out)"

# An entry whose offset lies past the end of the file is listed, and once
# read, reported as UnZip reports it (3), the others read as they come;
# one in the central directory or the end record is an overlap (12),
# found before any entry is read, as Debian's UnZip finds it once it
# gets there, where Apple's finds no local header there (2)
for z in pasteof pastfar pastmax pastcd pastend; do
    same -l $z.zip
    same -v $z.zip
done
for z in pasteof pastfar pastmax; do
    same -t $z.zip
    same -tq $z.zip
    same -p $z.zip
done
for z in pastcd pastend; do
    printf 'Archive:  %s.zip\nerror: invalid zip file with overlapped components (possible zip bomb)\n' $z >want
    ours 12 want none -t $z.zip
    [ $debian = 0 ] || same -tq $z.zip
    same -t $z.zip a.txt c.txt
done

# An archive over 4 GiB written without Zip64, its offsets wrapped to 32
# bits (sparse, 4.5 GiB), each entry found where it is, as 7-Zip finds
# it, with UnZip's warning of the 4 GiB its offsets leave out (1), where
# UnZip finds the first entry, and the first past 4 GiB, only by
# re-compensating (2), and Debian's stops at the second ("not enough
# memory for bomb detection", 4); a second block of entries, past 4 GiB,
# is unwrapped as the first is, and the last, selected alone, is where
# UnZip finds it (only with sparse files, above)
if [ -n "$holes" ]; then
    same -l offwrap.zip
    same -v offwrap.zip
    same -t offwrap.zip small
    same -p offwrap.zip small
    { echo 'warning [offwrap.zip]:  4294967296 extra bytes at beginning or within zipfile'
      echo '  (attempting to process anyway)'
      echo 'No errors detected in compressed data of offwrap.zip.'; } >want
    ours 1 want none -tq offwrap.zip
    # but 4 GiB before an archive, its offsets from its start, are data
    # that its offsets leave out, as UnZip takes them
    same -l prefix4g.zip
    same -t prefix4g.zip
fi

# Departure: a central header whose Zip64 extra lacks a field that its
# sizes call for ends the central directory there, where UnZip warns
# ("extra field (type: 0x0001) corrupt") and reads on
{ echo 'Archive:  short64.zip'
  echo 'error:  expected central file header signature not found (file #2).'
  echo '  (please check that you have transferred or created the zipfile in the'
  echo '  appropriate BINARY mode and that you have compiled UnZip properly)'
  printf '    testing: %-22s   OK\n' b
  echo 'At least one error was detected in short64.zip.'; } >want
ours 3 want none -t short64.zip

# Output beyond an entry's size is not written: an overrun, which UnZip
# writes out before finding the CRC wrong
printf 'hello hell' >want
"$U" -p overrun.zip >got 2>got.err && fail "-p overrun.zip: status 0"
cmp -s want got || fail "-p overrun.zip output"

# An entry of 5 GiB and more whose size, without Zip64, wrapped to 32
# bits: its data is all of it, as UnZip finds, listed by the wrapped size
for z in wrapped wrappedoff wrapped64; do
    same -l $z.zip
    same -v $z.zip
done
same -t wrapped.zip
{ echo 0 >got.st; "$U" -p wrapped.zip 2>got.err || echo $? >got.st; } |
    wc -c >got
[ "$(tr -d ' ' <got)" = 5368721471 ] && [ "$(cat got.st)" = 0 ] &&
    [ ! -s got.err ] || fail "-p wrapped.zip: $(cat got got.err)"
# Departure: but with a length not its size modulo 4 GiB, or that size in
# a Zip64 extra, an overrun, its output stopping at its size, where UnZip
# checks the CRC alone and finds the data good
for z in wrappedoff wrapped64; do
    { printf 'Archive:  %s.zip\n    testing: %-22s  \n' $z big
      printf '  error:  invalid compressed data to inflate\n'
      printf '    testing: %-22s   OK\n' small
      printf 'At least one error was detected in %s.zip.\n' $z; } >want
    ours 2 want none -t $z.zip
done
{ echo 0 >got.st; "$U" -p wrapped64.zip big 2>got.err || echo $? >got.st; } |
    wc -c >got
[ "$(tr -d ' ' <got)" = 1073754169 ] && [ "$(cat got.st)" = 2 ] &&
    grep -q invalid got.err || fail "-p wrapped64.zip: $(cat got got.err)"

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

# ---- Extraction -----------------------------------------------------

# Ours alone, as xboth runs it, with its status, output, and errors
# exactly: status, a file of the expected output, a file of the expected
# errors, then the arguments, leaving its tree in ours.tree.
xours() {
    want=$1
    wout=$2
    werr=$3
    shift 3
    [ ! -e x.ours ] || chmod -R u+rwx x.ours
    rm -rf x.ours
    mkdir x.ours
    [ -z "$SETUP" ] || (cd x.ours && eval "$SETUP") || fail "setup: $SETUP"
    set +e
    (cd x.ours && exec "$U" "$@") <"${IN:-/dev/null}" >ours.out 2>ours.err
    got=$?
    set -e
    [ "$got" = "$want" ] ||
        fail "expected status $want, got $got: $*: $(cat ours.err)"
    cmp -s "$wout" ours.out || fail "output: $*: $(diff "$wout" ours.out)"
    cmp -s "$werr" ours.err || fail "errors: $*: $(diff "$werr" ours.err)"
    tree x.ours >ours.tree
}

# Trees of directories, files, and links: modes from Unix (as they are)
# and MS-DOS (taking the umask), set-ID and sticky bits (-K), times from
# extended timestamps and DOS times (local time, in a zone with daylight
# saving time too), and the options that change them
SETUP=
IN=
for mask in 022 077; do
    umask $mask
    for opts in "" -q -qq -K -j -D -DD -n -o -V; do
        case $opts in
        -D) TIMES=files;;   # directories' times are left
        -DD) TIMES=none;;
        *) TIMES=all;;
        esac
        xsame $opts ../tree.zip
    done
done
umask 022
TIMES=all
export TZ='EST5EDT,M3.2.0,M11.1.0'
xsame ../tree.zip
xsame -K ../tree.zip
export TZ=UTC
# From here, directories that no entry makes have the time they were made,
# which may differ between the runs
TIMES=files
xsame ../implicit.zip

# Archives as for listing and testing, including Zip64 fields in each
# entry, data descriptors, and offsets shifted
for z in basic sfx sfxok zip64 zip64entries desc storedsize localname \
         badextra truncated badcdoff badheader badlocal multi empty plain \
         count2 count5 count0 sfx64 sfx64ok cmtnone cdjunk; do
    xsame ../$z.zip
done
xsame ../basic.zip -x 'dir/*'
xsame ../basic.zip 'dir/*' nomatch
xsame ../deflate64.zip

# Names that would reach outside the destination, or name nothing: ".."
# dropped, absolute paths made relative, '\' from MS-DOS, control
# characters, ";N" (-V); and links that later entries would write
# through: those entries fail, as in UnZip
for z in traversal linkfile linkabs linkdir linkdup linklink linkempty; do
    xsame ../$z.zip
    xsame -q ../$z.zip
done
xsame -j ../traversal.zip
xsame -V ../traversal.zip
[ $apple = 1 ] || xsame -o ../linkdup.zip  # (Apple's: status 2)
# A placeholder replaced: the later link made, or the empty file left
# (Apple's: status 2, as above)
[ $apple = 1 ] || xsame -o ../linklink.zip
[ $apple = 1 ] || xsame -o ../linkempty.zip
mkdir outside
[ -z "$(ls outside)" ] || fail "written outside: $(ls outside)"

# Departure: nothing is written through a link already in the
# destination, where UnZip follows it to a directory, but for the -d
# directory and its ancestors; a link where a file goes is replaced
# (with -o), as by UnZip
SETUP='ln -s ../outside pre'
printf 'Archive:  ../plain.zip\n  inflating: f                       \n' >want
cat >want.err <<EOF
checkdir error:  pre exists but is not directory
                 unable to process pre/x.txt.
EOF
xours 2 want want.err ../plain.zip
chmod -R u+rwx x.ours && rm -rf x.ours
mkdir -p x.ours/top
ln -s ../../outside x.ours/top/sub
set +e
(cd x.ours && exec "$U" -q ../tree.zip) >ours.out 2>ours.err </dev/null
st=$?
set -e
[ $st = 2 ] &&
    grep -q '^checkdir error:  top/sub exists but is not directory$' ours.err ||
    fail "tree.zip with a link inside: $st $(cat ours.err)"
SETUP='mkdir real && ln -s real lnk'
xsame ../plain.zip -d lnk
xsame ../plain.zip -d lnk/new
echo keep >outside/f
SETUP='ln -s ../outside/f f'
xsame -o ../plain.zip
xsame -n ../plain.zip
SETUP='ln -s nowhere f'
xsame -o ../plain.zip
[ "$(ls outside)" = f ] && [ "$(cat outside/f)" = keep ] ||
    fail "written through a link: $(ls outside)"
SETUP=

# The -d directory: made, a level at most; a '/' after it dropped (as
# Apple's does not); none with -f (UnZip freshens the current directory)
xsame ../plain.zip -d new
xsame ../plain.zip -dnew
xsame ../plain.zip -d new/two
[ $apple = 1 ] || xsame ../plain.zip -d new/
xsame -f ../plain.zip -d new
# Departure: -f with a missing -d directory freshens nothing, where
# UnZip, not making it, freshens the current directory (as Debian's
# does; Apple's freshens nothing too)
SETUP='printf old >f && touch -t 201001010000 f'
xboth -fo ../plain.zip -d new
printf 'Archive:  ../plain.zip\n' >want
[ "$(cat ours.st)" = 0 ] && cmp -s want ours.out && [ ! -s ours.err ] ||
    fail "-fo -d new: $(cat ours.st) $(cat ours.out ours.err)"
[ "$(cat x.ours/f)" = old ] && [ "$(ls x.ours)" = f ] ||
    fail "-fo -d new freshened: $(cat ours.tree)"
[ $debian = 0 ] || [ "$(cat x.ref/f)" = f ] ||
    fail "-fo -d new: REF freshened nothing: $(cat ref.tree)"
SETUP=': >file && touch -t 202001010000 file'
[ $apple = 1 ] || xsame ../plain.zip -d file  # (Apple's says nothing)

# What is in the way: a directory unwritable (unless root), a directory
# where a file goes, a file where a directory goes
SETUP='mkdir p && chmod 555 p'
xsame ../prompt.zip
SETUP='mkdir -p p/a.txt/x'
xsame -o ../prompt.zip
SETUP=': >p && touch -t 202001010000 p'
xsame ../prompt.zip

# Overwriting: the prompt, its answers read as UnZip reads them (y, n, A
# for all, N for none, r to rename, asking again for an empty name, and
# answers it does not take, a long one in pieces), and its end ("None"),
# with -f, -u, -n, and -o deciding first by the files' times
SETUP='"$REF" -qo ../prompt.zip && touch -t 202001010000 p/b.txt &&
       touch -t 203001010000 p/c.txt && printf old >p/e.txt &&
       touch -t 202501010000 p/e.txt'
n=0
for answers in 'y\nn\nA\n' 'n\ny\nN\n' 'r\nrenamed.txt\nN\n' \
               'r\n\n\nsub/new.txt\nA\n' 'x\nyes\nno\n' \
               '\nmaybe-not-valid-answer\nN\n' 'y\n' '' 'A' \
               'r\n../up/a.txt\nN\n' 'r\n/abs/a.txt\nN\n' 'r\n'; do
    n=$((n + 1))
    printf "$answers" >answers.$n
    IN=answers.$n
    opts='"" -q'
    [ $n -gt 2 ] || opts='"" -q -f -u -n -o -fo -uo -j'
    eval "set -- $opts"
    for opt; do
        # (Apple's takes a new name of "/abs" as absolute, and with none
        # read, renames the file to its own full path)
        [ $apple = 1 ] && [ $n -ge 11 ] && continue
        xsame $opt ../prompt.zip
    done
done
IN=
SETUP=

# Standard input as the archive (busybox's): -n, unless -o, as the
# answers to a prompt would come from the archive
IN=basic.zip
printf 'Archive:  -\nThe archive comment.\n   creating: dir/\n' >want.dir
printf '  inflating: %-22s  \n extracting: %-22s  \n extracting: %-22s  \n  inflating: %-22s  \n' \
    dir/text.txt dir/stored.txt empty ut.txt >want.files
cat want.dir want.files >want
xours 0 want none -
SETUP='"$U" -q ../basic.zip'
head -n 2 want.dir >want.none
xours 0 want.none none -
cat want.none want.files >want
xours 0 want none -o -
IN=
SETUP=

# The names that the Unicode path field or UTF-8 give (Apple's has no
# Unicode support, and others name files in a UTF-8 locale alone), and
# owners (-X, as Apple's does not restore them)
utf8=$(locale -a 2>/dev/null | grep -i '^c\.utf-*8$' | head -n 1 || true)
if [ $apple = 1 ] || [ -z "$utf8" ]; then
    xboth ../names.zip
    grep -q '^F ./café.txt ' ours.tree || fail "names.zip: $(cat ours.tree)"
fi
if [ $apple = 0 ]; then
    if [ -n "$utf8" ]; then
        export LC_ALL=$utf8
        xsame ../names.zip
        export LC_ALL=C
    fi
    xsame -X ../owners.zip
    xsame -qX ../owners.zip
fi

# Departure: a bad CRC, invalid data, or data beyond an entry's size
# leaves no file (UnZip keeps what it wrote); the messages and statuses
# are UnZip's (Apple's adds a CRC to the invalid data's)
for z in badcrc baddata truncdata overrun; do
    xboth ../$z.zip
    cmp -s ours.st ref.st && cmp -s ours.out ref.out ||
        fail "$z.zip: $(cat ours.st ours.out ours.err)"
    [ $apple = 1 ] || cmp -s ours.err ref.err ||
        fail "$z.zip errors: $(diff ref.err ours.err)"
    ! grep -v ' ./good.txt ' ours.tree | grep -q . ||
        fail "$z.zip kept: $(cat ours.tree)"
    grep -v ' ./good.txt ' ref.tree | grep -q . || fail "$z.zip: REF's"
done

# Entries skipped: encrypted, and methods other than stored, deflated,
# and Deflate64 (which REF decodes)
{ printf 'Archive:  ../methods.zip\n'
  printf '  inflating: %-22s  \n' ok.txt d64.bin
  printf ' extracting: %-22s  \n' last.txt; } >want
cat >want.err <<EOF
   skipping: enc.txt                 encrypted (not supported)
   skipping: bzip2.bin               \`bzip2' method not supported
   skipping: lzma.bin                \`LZMA' method not supported
   skipping: shrunk.bin              \`shrink' method not supported
   skipping: aes.bin                 unsupported compression method 99
   skipping: new.bin                 need PK compat. v6.3 (can do v4.5)
EOF
xours 81 want want.err ../methods.zip
[ "$(cut -d' ' -f1-2 ours.tree)" = "F ./d64.bin
F ./last.txt
F ./ok.txt" ] || fail "methods.zip: $(cat ours.tree)"
printf 'Archive:  ../encrypted.zip\n' >want
printf '   skipping: enc.txt                 encrypted (not supported)\n' \
    >want.err
xours 81 want want.err ../encrypted.zip
[ ! -s ours.tree ] || fail "encrypted.zip: $(cat ours.tree)"

# Overlapped components, a zip bomb's, found before anything is written
for z in overlap inner overlapcd csizepast; do
    printf 'Archive:  ../%s.zip\n' $z >want
    echo 'error: invalid zip file with overlapped components (possible zip bomb)' >want.err
    xours 12 want want.err ../$z.zip
    [ ! -s ours.tree ] || fail "$z.zip: $(cat ours.tree)"
done
# An entry whose offset lies past the end of the file is reported, the
# others extracted
xsame ../pasteof.zip
xsame ../pastfar.zip

# A write that fails, here past a limit on file sizes (its signal
# ignored), asks as UnZip does whether to go on (y), else stops, the
# file discarded (a departure: UnZip keeps what it wrote), status 50
printf 'Archive:  ../large.zip\n  inflating: large.txt               ' >want
printf '\nlarge.txt:  write error (disk full?).  Continue? (y/n/^C) ' \
    >want.err
for answer in n y; do
    chmod -R u+rwx x.ours && rm -rf x.ours
    mkdir x.ours
    printf '%s\n' $answer >answers
    set +e
    (trap '' XFSZ; ulimit -f 16 && cd x.ours && exec "$U" ../large.zip) \
        <answers >ours.out 2>ours.err
    st=$?
    set -e
    [ $st = 50 ] && cmp -s want.err ours.err ||
        fail "write error ($answer): $st $(cat ours.err)"
    [ ! -e x.ours/large.txt ] || fail "write error: file kept"
    if [ $answer = n ]; then
        cmp -s want ours.out || fail "write error: $(cat ours.out)"
        [ ! -e x.ours/after.txt ] || fail "write error: went on"
    else
        [ -e x.ours/after.txt ] || fail "write error: stopped"
    fi
done

# A write to standard output (-p, -c) that fails, here to /dev/full where
# there is one, is reported for the entry whose data it held, however
# short, status 50, where UnZip ignores it (a departure)
if [ -w /dev/full ]; then
    printf 'after.txt:  write error (disk full?).\n' >want.err
    for opt in -p -c; do
        set +e
        "$U" $opt large.zip after.txt </dev/null >/dev/full 2>ours.err
        st=$?
        set -e
        [ $st = 50 ] && cmp -s want.err ours.err ||
            fail "write error to /dev/full ($opt): $st $(cat ours.err)"
    done
fi

# Departure: an interrupt (SIGINT) removes the file being written, and
# unzip dies by the signal, as gzip and zip do, where UnZip exits 80.
# A non-interactive shell's background jobs ignore SIGINT, so unzip runs
# in the foreground, writing 256 MiB of zeros, and a background watcher
# stops it once the file appears, notes its size, and interrupts it.
# (When unzip ends too soon to be stopped, or this shell was started
# ignoring SIGINT, there is nothing to check.)
if [ -n "$(sh -c 'kill -INT $$; echo ignored' 2>/dev/null)" ]; then
    echo "unzip.sh: SIGINT is ignored here; interrupt test skipped" >&2
else
    $PY - intr.zip <<'EOF' || fail "intr.zip"
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    info = zipfile.ZipInfo("zeros", (2020, 1, 2, 3, 4, 6))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    with z.open(info, "w") as f:
        for i in range(256):
            f.write(bytes(1 << 20))
EOF
    chmod -R u+rwx x.ours && rm -rf x.ours
    mkdir x.ours
    rm -f intr.pid intr.seen
    { n=0
      while [ ! -s intr.pid ] || [ ! -e x.ours/zeros ]; do
          n=$((n + 1))
          [ $n -lt 1000000 ] || exit 0
      done
      read pid <intr.pid
      kill -STOP $pid 2>/dev/null || exit 0
      wc -c <x.ours/zeros | tr -d ' ' >intr.seen
      kill -INT $pid
      kill -CONT $pid; } &
    watcher=$!
    set +e
    (cd x.ours && exec sh -c 'echo $$ >../intr.pid && exec "$@"' sh \
        "$U" -q ../intr.zip) >ours.out 2>ours.err </dev/null
    st=$?
    set -e
    wait $watcher
    if [ -e intr.seen ] && [ "$(cat intr.seen)" -lt 268435456 ]; then
        [ $st = 130 ] || fail "not interrupted: $st $(cat ours.err)"
        [ -z "$(ls x.ours)" ] ||
            fail "partial output left on interrupt: $(ls -l x.ours)"
    else
        echo "unzip.sh: unzip finished before it could be interrupted" >&2
    fi
    chmod -R u+rwx x.ours && rm -rf x.ours intr.zip
fi

# Out of memory: an archive's memory, its planned paths included, is
# claimed before anything is extracted, so that running out (status 4)
# leaves nothing, not even the -d directory. Systems limit address space
# (ulimit -v), and Linux also private writable memory (ulimit -d). A
# small run fits under each, while 20,000 links, whose targets of 4,096
# bytes are held to the end in room claimed beforehand, need over 80 MB
# (their local headers, sparse, are never reached). macOS's shell sets
# neither limit, and builds with sanitizers that reserve shadow memory
# cannot run under them, nor can emulators, which the probe finds.
# UNZIPOOM names another build for these tests, as ctest, whose $U is
# sanitized, gives one.
oomunzip=${UNZIPOOM:-$U}
oomskip=
if LC_ALL=C grep -aq -e __asan_ -e __hwasan_ -e __msan_ -e __tsan_ \
                     "$oomunzip"; then
    oomskip="sanitized"
elif ! (ulimit -v 60000) 2>/dev/null; then
    oomskip="ulimit -v unsupported"
else
    $PY - oombig.zip <<'EOF'
import struct, sys
n, size = 20000, 4096
local = 30 + 10 + size
cd = bytearray()
for i in range(n):
    name = b"d/%08d" % i
    cd += struct.pack("<IHHHHIIIIHHHHHII", 0x02014B50, 3 << 8 | 30, 20, 0,
                      0, 0, 0, size, size, len(name), 0, 0, 0, 0,
                      0o120777 << 16, i * local) + name
f = open(sys.argv[1], "wb")
f.truncate(n * local)
f.seek(n * local)
f.write(cd)
f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, n, n, len(cd),
                    n * local, 0))
EOF
fi
printf 'error:  not enough memory\n' >want.err
for limit in "-v 60000" "-d 30000"; do
    [ -z "$oomskip" ] || break
    [ "$limit" != "-d 30000" ] || [ "$(uname -s)" = Linux ] || continue
    set +e
    (ulimit $limit && exec "$oomunzip" -v) >out 2>err
    st=$?
    set -e
    if [ $st != 0 ] && [ $st != 4 ] && [ $st -lt 128 ]; then
        oomskip="no start under ulimit $limit: $(head -n 1 err)"
        break
    fi
    grep -q '^tugz unzip' out ||
        fail "unzip -v under ulimit $limit: $st $(cat err)"
    for run in "0 ../tree.zip" "4 ../oombig.zip -d new"; do
        [ ! -e x.ours ] || chmod -R u+rwx x.ours
        rm -rf x.ours
        mkdir x.ours
        set +e
        (ulimit $limit && cd x.ours && exec "$oomunzip" -q ${run#* }) \
            >out 2>err </dev/null
        st=$?
        set -e
        [ $st = "${run%% *}" ] ||
            fail "ulimit $limit, ${run#* }: status $st $(cat err)"
    done
    cmp -s want.err err || fail "ulimit $limit: $(cat err)"
    [ -z "$(ls x.ours)" ] || fail "ulimit $limit left: $(ls x.ours)"
done
[ -z "$oomskip" ] || echo "unzip.sh: out-of-memory tests skipped: $oomskip" >&2
rm -f oombig.zip

if [ -n "$SLOW" ]; then
    # Zip64: entries of 4 GiB and a MiB, stored, pushing the entry after
    # it past 4 GiB, and deflated, extracted one at a time
    $PY - big.zip <<'EOF' || fail "big.zip"
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
chunk = bytes(1 << 20)
for name, method in (("stored.bin", zipfile.ZIP_STORED),
                     ("deflated.bin", zipfile.ZIP_DEFLATED)):
    info = zipfile.ZipInfo(name, (2020, 1, 2, 3, 4, 6))
    info.compress_type = method
    info.external_attr = 0o100644 << 16
    with z.open(info, "w", force_zip64=True) as f:
        for i in range(4097):
            f.write(chunk)
z.writestr(zipfile.ZipInfo("after.txt", (2020, 1, 2, 3, 4, 6)), b"after\n")
z.close()
EOF
    for member in stored.bin deflated.bin; do
        [ ! -e x.ours ] || chmod -R u+rwx x.ours
        rm -rf x.ours
        mkdir x.ours
        (cd x.ours && exec "$U" -q ../big.zip $member after.txt) ||
            fail "big.zip $member"
        [ "$(wc -c <x.ours/$member | tr -d ' ')" = 4296015872 ] ||
            fail "big.zip $member: $(ls -l x.ours)"
        [ "$(cat x.ours/after.txt)" = after ] || fail "big.zip after.txt"
    done
    chmod -R u+rwx x.ours && rm -rf x.ours big.zip

    # The entry whose size wrapped (above), extracted whole, as UnZip
    # extracts it
    xsame ../wrapped.zip
    rm -rf x.ours x.ref

    # The archive whose offsets wrapped (above), extracted as UnZip
    # extracts it (whose t/ no entry makes), but for its re-compensating
    # (2), as Apple's does, where Debian's stops at the second entry (only
    # with sparse files, above)
    if [ -z "$holes" ]; then
        :
    elif [ $debian = 0 ]; then
        TIMES=files
        xboth -q ../offwrap.zip
        TIMES=all
        cmp -s ours.tree ref.tree ||
            fail "offwrap.zip tree: $(diff ref.tree ours.tree)"
    else
        rm -rf x.ours
        mkdir x.ours
        set +e
        (cd x.ours && exec "$U" -q ../offwrap.zip) >ours.out 2>ours.err
        echo $? >ours.st
        set -e
        [ "$(wc -c <x.ours/f1 | tr -d ' ')" = 2415919106 ] &&
            [ "$(cat x.ours/small)" = hello ] &&
            [ "$(cat x.ours/t/16383)" = 16383 ] ||
            fail "offwrap.zip: $(ls -l x.ours)"
    fi
    if [ -n "$holes" ]; then
        printf '%s\n' \
            'warning [../offwrap.zip]:  4294967296 extra bytes at beginning or within zipfile' \
            '  (attempting to process anyway)' >want.err
        [ "$(cat ours.st)" = 1 ] && [ ! -s ours.out ] &&
            cmp -s want.err ours.err ||
            fail "offwrap.zip: status $(cat ours.st): $(cat ours.out ours.err)"
        rm -rf x.ours x.ref
    fi
fi

[ ! -e "$tmp/FAILED" ] || exit 1
