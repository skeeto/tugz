#!/bin/sh
# Windows end-to-end tests of zip.exe, run under w64devkit. Archives are
# extracted by Windows' own readers: Explorer's zip folder (the decoder
# in libdeflate issue #323), .NET's Expand-Archive, and tar (libarchive).
# Also covers wildcards, hidden files, Unicode names, and long paths.
# Usage: sh test/zip_windows.sh ./zip.exe
set -e

unset ZIPOPT ZIP  # options for zip, and ZIP unexported for the binary
ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
testdir=$(cd "$(dirname "$0")" && pwd)
TAR=C:/Windows/System32/tar.exe
tmp=$(mktemp -d)
trap 'cd / && rm -rf "$tmp"' EXIT
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
    "$@" >/dev/null 2>&1
    got=$?
    set -e
    [ "$got" = "$want" ] || fail "expected status $want, got $got: $*"
}

# Archive names, as tar lists them (with CRLF line endings), failing the
# test where it cannot read the archive
list() {
    listed=$("$TAR" -tf "$1" 2>"$tmp/list.err") ||
        fail "tar -tf $1: $(cat "$tmp/list.err")"
    [ -z "$listed" ] || printf '%s\n' "$listed" | tr -d '\r'
}

ps() {
    powershell -NoProfile -NonInteractive -Command "$1"
}

# Extract with Explorer's zip folder, which runs asynchronously.
shell_extract() {  # archive directory count
    mkdir "$2"
    ps "\$s = New-Object -ComObject Shell.Application;
        \$src = \$s.NameSpace((Resolve-Path '$1').Path);
        \$dst = \$s.NameSpace((Resolve-Path '$2').Path);
        \$dst.CopyHere(\$src.Items(), 0x614);
        for (\$i = 0; \$i -lt 100; \$i++) {
            \$n = (Get-ChildItem -Recurse -File '$2').Count;
            if (\$n -ge $3) { break }
            Start-Sleep -Milliseconds 100
        }"
}

# Inputs: the literal-only file is a de Bruijn sequence, in which no
# three-byte string repeats, so its compressed blocks use no distances.
# Explorer, which rejects incomplete codes, must extract it from t.zip
# (listing an archive inflates nothing).
mkdir -p tree/sub
printf 'hello hello hello hello\n' >tree/a.txt
printf b >tree/b.txt
: >tree/empty
printf x >tree/one
head -c 300000 /dev/urandom >tree/sub/random
awk 'BEGIN{for(i=0;i<50000;i++)print i, i*i, "lorem ipsum"}' >tree/sub/text
awk 'function db(t, p,  j) {
         if (t > 3) {
             if (3%p == 0) for (j = 1; j <= p; j++) printf "%c", 97+a[j]
         } else {
             a[t] = a[t-p]; db(t+1, p)
             for (j = a[t-p]+1; j < 26; j++) { a[t] = j; db(t+1, t) }
         }
     }
     BEGIN { a[0] = 0; db(1, 1) }' >tree/literals.txt
ps "Set-Content -LiteralPath ('tree\caf' + [char]0xe9 + '.txt') -Value u -NoNewline"
printf h >tree/hidden.txt
ps "(Get-Item tree\hidden.txt).Attributes = 'Hidden'"
printf s >tree/system.txt
ps "(Get-Item tree\system.txt).Attributes = 'System'"
mkdir want
cp -r tree want/
rm want/tree/hidden.txt want/tree/system.txt
nfiles=7  # files expected, plus the Unicode-named one

for level in 1 6 9; do
    rm -rf t.zip x1 x2 x3
    "$ZIP" -qr -$level t.zip tree

    ps "Expand-Archive -Path t.zip -DestinationPath x1" ||
        fail "Expand-Archive -$level"
    diff -r want/tree x1/tree >/dev/null || fail "Expand-Archive contents -$level"

    mkdir x2
    "$TAR" -xf t.zip -C x2 || fail "tar -x -$level"
    diff -r want/tree x2/tree >/dev/null || fail "tar contents -$level"

    shell_extract t.zip x3 $((nfiles + 1))
    diff -r want/tree x3/tree >/dev/null || fail "Explorer contents -$level"
done

# Hidden and system files: skipped when recursing, unless -S
"$ZIP" -qr h.zip tree
list h.zip | grep -q hidden && fail "hidden file included"
"$ZIP" -qrS h2.zip tree
list h2.zip | grep -q hidden.txt || fail "-S omitted hidden file"
list h2.zip | grep -q system.txt || fail "-S omitted system file"

# ...judged by the directory entry, as Info-ZIP does: a junction to a
# hidden directory is followed
mkdir hid jt
printf j >hid/j.txt
ps "\$d = Get-Item hid;
    \$d.Attributes = \$d.Attributes -bor [IO.FileAttributes]::Hidden;
    New-Item -ItemType Junction -Path jt\\link -Target \$d.FullName | Out-Null"
"$ZIP" -qr jt.zip jt
list jt.zip | grep -qx jt/link/j.txt || fail "junction to a hidden directory"

# ...and a junction to an ancestor is a loop, found by file identity
mkdir -p lp/sub
ps "New-Item -ItemType Junction -Path lp\\sub\\up -Target (Resolve-Path lp).Path |
    Out-Null"
"$ZIP" -r lp.zip lp 2>&1 | grep -q 'skipping directory loop: lp/sub/up' ||
    fail "junction loop"

# Departure: a dangling junction while recursing exits 18, unless -x
# leaves it out (Info-ZIP warns that the name is not matched, exiting 0)
mkdir -p dj/gone
printf x >dj/f
ps "New-Item -ItemType Junction -Path dj\\link -Target (Resolve-Path dj\\gone).Path |
    Out-Null"
rmdir dj/gone
expect_status 18 "$ZIP" -r dj.zip dj
"$ZIP" -qr dj2.zip dj -x dj/link 2>err || fail "excluded dangling junction"
[ ! -s err ] || fail "excluded dangling junction: $(cat err)"

# Recursion describes files from the listing, but still examines those
# the archive's size: the archive is left out, a copy of it is not, nor
# is a hard link to it
mkdir self
printf x >self/x
"$ZIP" -qr self/s.zip self
cp self/s.zip self/copy.zip
"$ZIP" -qr self/s.zip self
list self/s.zip | grep -qx self/s.zip && fail "archive added to itself"
list self/s.zip | grep -qx self/copy.zip || fail "same-size file left out"
ps "New-Item -ItemType HardLink -Path self\\link.zip -Target (Resolve-Path self\\s.zip).Path |
    Out-Null"
"$ZIP" -qr self/s.zip self
list self/s.zip | grep -q link.zip && fail "archive added through a hard link"

# Wildcards are expanded by zip, case-insensitively, either separator
"$ZIP" -q w1.zip 'tree/*.txt'
list w1.zip | grep -v caf | sort >got  # tar prints names in the ANSI code page
printf 'tree/a.txt\ntree/b.txt\ntree/literals.txt\n' >want.txt
cmp -s got want.txt || fail "wildcard: $(cat got)"
[ "$(list w1.zip | wc -l)" = 4 ] || fail "wildcard count"
"$ZIP" -q w2.zip 'TREE\A.*' 'tree\s?b\*'
list w2.zip >got
printf 'TREE/a.txt\ntree/sub/random\ntree/sub/text\n' >want.txt
cmp -s got want.txt || fail "wildcard case and separators: $(cat got)"
expect_status 12 "$ZIP" w3.zip 'tree/*.none'
# ...after a device prefix too, whose ? is no wildcard
here=$(pwd)
win=$(printf %s "$here" | tr / '\\')
for p in "//?/$here/tree/*.txt" "\\\\?\\$win\\tree\\*.txt"; do
    rm -f w6.zip
    "$ZIP" -q w6.zip "$p" || fail "wildcard $p: status $?"
    list w6.zip | grep -v caf | sed 's|.*/tree/|tree/|' | sort >got
    printf 'tree/a.txt\ntree/b.txt\ntree/literals.txt\n' >want.txt
    cmp -s got want.txt || fail "wildcard $p: $(cat got)"
done

# ...as Info-ZIP's dosmatch does: a name without a period matches as if
# it ended in one, so *.* matches every (unhidden) name
"$ZIP" -q w4.zip 'tree/*.*'
list w4.zip | grep -v caf | sort >got
printf '%s\n' tree/a.txt tree/b.txt tree/empty tree/literals.txt tree/one \
    tree/sub/ >want.txt
cmp -s got want.txt || fail "*.*: $(cat got)"
"$ZIP" -q w5.zip 'tree/one.*'
[ "$(list w5.zip)" = tree/one ] || fail "name.*: $(list w5.zip)"

# Freshening matches wildcards against entries rather than the disk, as
# Info-ZIP's port does: case matters, * spans directories, and the
# current directory need have no matches (here it has want.txt)
mkdir -p fr/sub
printf 1 >fr/a.txt
printf 2 >fr/sub/b.txt
"$ZIP" -qr fr.zip fr
ps "foreach (\$f in 'fr\\a.txt', 'fr\\sub\\b.txt') {
        (Get-Item \$f).LastWriteTime = '2030-01-01' }"
expect_status 12 "$ZIP" -f fr.zip '*.TXT'
"$ZIP" -f fr.zip '*.txt' >out
grep -c '^freshening: ' out | grep -qx 2 || fail "-f '*.txt': $(cat out)"
list fr.zip | grep -q want.txt && fail "-f added a file"

# Departure: entries select files only within the current directory,
# not by .. components nor through a junction, nor by a junction or
# link at their end, which is not followed, with a warning, all of which
# Info-ZIP's port follows (by its sources), though their files can be
# named as paths (making links needs Developer Mode or elevation)
mkdir -p esc/w/d esc/out
printf secret >esc/secret
printf key >esc/out/key
printf g >esc/w/d/g
ps "New-Item -ItemType Junction -Path esc\\w\\junc -Target (Resolve-Path esc\\out).Path |
    Out-Null
    New-Item -ItemType Junction -Path esc\\w\\jd -Target (Resolve-Path esc\\out).Path |
    Out-Null"
links=jd
if (cd esc/w && cmd /c 'mklink fl ..\secret') >/dev/null 2>&1; then
    links="jd fl"
fi
(cd esc/w && "$ZIP" -q e.zip ../secret junc/key d/g $links)
ps "foreach (\$f in 'esc\\secret', 'esc\\out\\key', 'esc\\w\\d\\g') {
        (Get-Item \$f).LastWriteTime = '2030-01-01' }"
(cd esc/w && "$ZIP" -u e.zip >../log 2>../err) || fail "-u of escaping names"
[ "$(cat esc/log)" = "updating: d/g (stored 0%)" ] ||
    fail "-u of escaping names: $(cat esc/log)"
for l in $links; do
    grep -qx "zip warning: not following link that an entry names: $l" \
        esc/err || fail "-u of a final link $l: $(cat esc/err)"
done
(cd esc/w && "$ZIP" -f e.zip '*' >../log 2>../err) ||
    fail "-f of escaping names"
[ ! -s esc/log ] || fail "-f '*' of escaping names: $(cat esc/log)"
[ "$(grep -c 'not following link' esc/err)" = "$(set -- $links; echo $#)" ] ||
    fail "-f '*' of final links: $(cat esc/err)"
named="../secret junc/key"
case $links in *fl*) named="$named fl" ;; esac
(cd esc/w && "$ZIP" -u e.zip $named >../log) || fail "-u of named paths"
[ "$(grep -c '^updating: ' esc/log)" = "$(set -- $named; echo $#)" ] ||
    fail "-u of named paths: $(cat esc/log)"

# -nw leaves ? a wildcard, as in Info-ZIP
"$ZIP" -q -nw nw.zip 'tree/?.txt'
list nw.zip | sort >got
printf 'tree/a.txt\ntree/b.txt\n' >want.txt
cmp -s got want.txt || fail "-nw ?: $(cat got)"
expect_status 12 "$ZIP" -nw nw2.zip 'tree/*.txt'

# ? matches a character, not a byte of its UTF-8, as Info-ZIP's port
# matches characters, both on disk and in filters
mkdir u8
ps "Set-Content -LiteralPath ('u8\\' + [char]0x20ac + '.txt') -Value e"
"$ZIP" u81.zip 'u8/?.txt' >out
grep -q "^  adding: u8/$(printf '\342\202\254').txt" out ||
    fail "? as a character: $(cat out)"
expect_status 12 "$ZIP" u82.zip 'u8/*' -x 'u8/?.txt'

# Wildcards ignore case beyond ASCII too, as the file system and
# Info-ZIP's port (by towupper) compare names, here given as UTF-8 by
# -@: E-acute finds e-acute, Cyrillic A finds a. Filters, as the port's
# narrow dosmatch, by its sources, ignore only ASCII case.
mkdir uc
ps "Set-Content -LiteralPath ('uc\\' + [char]0xe9 + 'cole.txt') -Value e;
    Set-Content -LiteralPath ('uc\\' + [char]0x430 + 'b.txt') -Value a"
printf 'uc/\303\211*.txt\nuc/\320\220?.TXT\n' | "$ZIP" uc1.zip -@ >out
grep -q "^  adding: uc/$(printf '\303\251')cole.txt" out &&
    grep -q "^  adding: uc/$(printf '\320\260')b.txt" out ||
    fail "case beyond ASCII: $(cat out)"
printf 'uc/\303\211*\n' >uc.lst
"$ZIP" uc2.zip 'uc/*' -x@uc.lst >out
[ "$(grep -c '^  adding: ' out)" = 2 ] || fail "-x beyond ASCII: $(cat out)"

# A hidden or system file is left out even when named, by a wildcard
# or in a -@ list, unless -S
expect_status 12 "$ZIP" h3.zip tree/hidden.txt
"$ZIP" -qS h4.zip tree/hidden.txt tree/system.txt
[ "$(list h4.zip | wc -l)" = 2 ] || fail "-S with named hidden files"
printf 'tree/hidden.txt\ntree/one\n' | "$ZIP" -q h5.zip -@
[ "$(list h5.zip)" = tree/one ] || fail "-@ hidden file: $(list h5.zip)"
"$ZIP" -qS h6.zip 'tree/*.txt'
list h6.zip | grep -q system.txt || fail "-S with a wildcard"

# Patterns and -d names are normalized as names are (backslashes, ./),
# and filters match as wildcards do, ignoring case and with DOS rules,
# except against entries (-d)
"$ZIP" -qr x1.zip tree -x 'tree\sub\*' '.\tree\*.TXT'
list x1.zip | sort >got
printf 'tree/\ntree/empty\ntree/one\n' >want.txt
cmp -s got want.txt || fail "-x with backslashes and case: $(cat got)"
"$ZIP" -q x2.zip tree/a.txt tree/one tree/b.txt
"$ZIP" -qd x2.zip 'tree\one' '.\tree\a.*'
[ "$(list x2.zip)" = tree/b.txt ] || fail "-d with backslashes: $(list x2.zip)"
expect_status 12 "$ZIP" -d x2.zip 'TREE/*'
# ...and a -d name without wildcards, though looked up by name, matches
# the same way: by DOS rules ("gone." names gone), but in case exactly
printf g >gone
printf g >Gone2
"$ZIP" -q x3.zip gone Gone2 tree/b.txt
rm gone Gone2
expect_status 12 "$ZIP" -d x3.zip gone2
"$ZIP" -qd x3.zip gone.
list x3.zip >got
printf 'Gone2\ntree/b.txt\n' >want.txt
cmp -s got want.txt || fail "-d of names not on disk: $(cat got)"

# A file replaces an entry whose name differs only in case, which keeps
# its name, as in Info-ZIP
"$ZIP" -q c1.zip tree/a.txt
"$ZIP" -q c1.zip TREE/A.TXT
[ "$(list c1.zip)" = tree/a.txt ] || fail "case-only update: $(list c1.zip)"

# Departure: paths that differ only in case are one path, added once
# under the first spelling, but different files whose names so differ
# collide (Info-ZIP's port, by its sources, adds both in either case)
"$ZIP" -q c2.zip TREE/A.TXT tree/a.txt 'Tree/*.txt'
list c2.zip | grep -v caf >got
printf 'TREE/A.TXT\nTree/b.txt\nTree/literals.txt\n' >want.txt
cmp -s got want.txt || fail "case-only repeat: $(cat got)"
printf 'tree/b.txt\r\nTREE/B.TXT\r\n' | "$ZIP" -q c3.zip -@
[ "$(list c3.zip)" = tree/b.txt ] || fail "case-only repeat (-@): $(list c3.zip)"
"$ZIP" -qr c4.zip tree/sub TREE/SUB
[ "$(list c4.zip | wc -l)" = 3 ] || fail "case-only repeat (-r)"
cp tree/b.txt tree/sub/B.TXT
expect_status 16 "$ZIP" -j c5.zip tree/b.txt tree/sub/B.TXT
rm tree/sub/B.TXT

# A file named as the archive's path is left out, ignoring case
mkdir ja
printf x >ja/c6.zip
expect_status 12 "$ZIP" -j C6.ZIP ja/c6.zip
[ ! -e c6.zip ] || fail "made an archive of a file named as it"

# Lists lose trailing spaces and periods, as with Info-ZIP's getnam,
# such as from cmd's "echo name > list"
printf 'tree/a.txt \r\ntree/one.\r\n' | "$ZIP" -q at2.zip -@
list at2.zip >got
printf 'tree/a.txt\ntree/one\n' >want.txt
cmp -s got want.txt || fail "-@ trailing spaces: $(cat got)"
printf '*.txt \r\n' >pat.lst
"$ZIP" -qr at3.zip tree -x@pat.lst
list at3.zip | grep -q a.txt && fail "@file trailing space"

# A directory as a pattern file fails, as the C runtime of Info-ZIP's
# port refuses to open one (on POSIX it is an empty list)
mkdir atdir
"$ZIP" atdir.zip tree/a.txt -x@atdir 2>err && fail "@dir list"
printf '%s\n' 'zip I/O error: Permission denied' \
    "zip error: File not found or no read permission (x pattern file '@atdir')" >want.txt
cmp -s err want.txt || fail "@dir list: $(cat err)"
expect_status 18 "$ZIP" atdir.zip tree/a.txt -x@atdir

# Paths are resolved before the \\?\ prefix, which turns off Win32
# parsing: "." and "..", doubled separators, root- and drive-relative
# paths, and a wildcard in the first component. An empty path is no file.
here=$(pwd)  # C:/...
(cd tree && "$ZIP" -qr ../p1.zip .) || fail "zip -r ../p1.zip ."
list p1.zip | grep -qx sub/random || fail "zip -r ../p1.zip . contents"
(cd tree/sub && "$ZIP" -q ../../p2.zip ../one) || fail "zip ../../p2.zip"
[ "$(list p2.zip)" = ../one ] || fail "..: $(list p2.zip)"
"$ZIP" -q p3.zip tree//one tree/sub/../b.txt "${here#?:}/tree/a.txt" \
    "${here%%:*}:tree/empty"
list p3.zip >got
printf 'tree/one\ntree/sub/../b.txt\n%s/tree/a.txt\ntree/empty\n' \
    "${here#?:/}" >want.txt
cmp -s got want.txt || fail "paths: $(cat got)"
(cd tree && "$ZIP" -q ../p4.zip '*.txt') || fail "zip ../p4.zip '*.txt'"
[ "$(list p4.zip | wc -l)" = 4 ] || fail "wildcard in the first component"
(cd tree && "$ZIP" -q ../p8.zip "${here%%:*}:*.txt") ||
    fail "zip ../p8.zip C:*.txt"
[ "$(list p8.zip | wc -l)" = 4 ] || fail "drive-relative wildcard"
# A bare drive is its current directory, whose entries are examined
# there, not at its root (where Info-ZIP's port, by its sources, joins
# them). Their names are unlike any at the root, should that recur.
mkdir -p bare/zq37d
printf a >bare/zq37a.txt
printf b >bare/zq37d/zq37b.txt
(cd bare && "$ZIP" -qr ../pdrive.zip "${here%%:*}:") ||
    fail "zip -r ../pdrive.zip C:"
[ "$(list pdrive.zip | tr '\n' ' ')" = "zq37a.txt zq37d/ zq37d/zq37b.txt " ] ||
    fail "zip -r C: $(list pdrive.zip)"
expect_status 12 "$ZIP" -r p5.zip ''

# Names drop a device or UNC prefix along with the drive, as Info-ZIP
"$ZIP" -q p6.zip "//?/$here/tree/one"
[ "$(list p6.zip)" = "${here#?:/}/tree/one" ] ||
    fail "device path name: $(list p6.zip)"

# A device path is resolved as Win32 resolves it, in any form except
# exactly \\?\, which Win32 takes as it is
win=$(printf %s "$here" | tr / '\\')
"$ZIP" -q p9.zip "//?/$here/tree/sub/../one" "\\\\?/$here/tree/./b.txt" \
    "//./$here/tree//a.txt" "\\\\?\\$win\\tree\\empty"
list p9.zip >got
printf '%s/tree/sub/../one\n%s/tree/./b.txt\n%s/tree/a.txt\n%s/tree/empty\n' \
    "${here#?:/}" "${here#?:/}" "${here#?:/}" "${here#?:/}" >want.txt
cmp -s got want.txt || fail "device paths: $(cat got)"
expect_status 12 "$ZIP" p10.zip "\\\\?\\$win\\tree\\sub\\..\\one"

# ...keeping trailing dots and spaces, even in \\.\ paths, which Win32
# would parse again, stripping them
mkdir dots
printf plain >dots/name
ps "Set-Content -NoNewline -LiteralPath '\\\\?\\$win\\dots\\name.' -Value dot
    Set-Content -NoNewline -LiteralPath '\\\\?\\$win\\dots\\sp ' -Value sp"
for p in "//./$here/dots" "\\\\.\\$win\\dots" dots; do
    rm -f dots.zip
    "$ZIP" -qr dots.zip "$p" || fail "zip -r $p"
    [ "$("$TAR" -xOf dots.zip '*/name.')$("$TAR" -xOf dots.zip '*/sp ')" = \
      dotsp ] || fail "zip -r $p: $(list dots.zip)"
done
ps "Remove-Item -LiteralPath '\\\\?\\$win\\dots' -Recurse -Force"

# ...but one missing its device, server, or share names nothing
expect_status 12 "$ZIP" p11.zip "//?/" "//./" "//?/UNC/localhost/" \
    "\\\\localhost\\" "//localhost//tree/one" "//"

# A share root is named as with its separator, as Info-ZIP names any
# directory: by nothing, so that its entries drop the whole prefix
share="//localhost/${here%%:*}\$"  # an administrative share, if shared
if "$ZIP" -q s1.zip "$share/${here#?:/}/tree/one" 2>/dev/null; then
    [ "$(list s1.zip)" = "${here#?:/}/tree/one" ] ||
        fail "share name: $(list s1.zip)"
    "$ZIP" -q s2.zip "$share" "\\\\?\\UNC\\localhost\\${here%%:*}\$" \
        tree/one || fail "share roots"
    [ "$(list s2.zip)" = tree/one ] || fail "share root: $(list s2.zip)"
fi

# An archive named by its volume's GUID, or by its device, as a link's
# target is named on a volume with no drive letter, or one unknown to
# the mount manager
vol=$(mountvol "${here%%:*}:\\" /L | tr -d ' \r')
"$ZIP" -q "$vol${here#?:/}/vg.zip" tree/a.txt
"$ZIP" -q "$vol${here#?:/}/vg.zip" tree/b.txt
[ "$(list vg.zip | tr '\n' ' ')" = "tree/a.txt tree/b.txt " ] ||
    fail "archive named by volume GUID: $(list vg.zip)"
cat >dev.cs <<'EOF'
using System.Runtime.InteropServices;
using System.Text;
public static class Dev {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint QueryDosDeviceW(string name, StringBuilder buf, uint n);
    public static string Of(string name) {
        StringBuilder b = new StringBuilder(1024);
        QueryDosDeviceW(name, b, 1024);
        return b.ToString();
    }
}
EOF
dev=$(ps "Add-Type -TypeDefinition (Get-Content -Raw dev.cs);
          [Dev]::Of('${here%%:*}:')" | tr -d '\r')
"$ZIP" -q "\\\\?\\GLOBALROOT$dev${win#?:}\\vd.zip" tree/a.txt
"$ZIP" -q "\\\\?\\GLOBALROOT$dev${win#?:}\\vd.zip" tree/b.txt
[ "$(list vd.zip | tr '\n' ' ')" = "tree/a.txt tree/b.txt " ] ||
    fail "archive named by device: $(list vd.zip)"

# Only a letter is a drive: "1:s" is stream s of file 1, and named so
ps "Set-Content -LiteralPath 1 -Value f;
    Set-Content -LiteralPath 1 -Stream s -Value s"
"$ZIP" -q p7.zip 1:s
[ "$(list p7.zip)" = 1:s ] || fail "stream name: $(list p7.zip)"

# A bare DOS device name is the device, a special file, but in a
# directory a file named like one (made elsewhere) is just a file. The
# console (CON), where there is one, refuses an open for attributes
# alone, but is special too.
"$ZIP" dv.zip NUL 2>&1 | grep -qF 'special file: NUL' || fail "NUL"
if "$ZIP" dv.zip 'CONIN$' 2>&1 | grep -qF 'special file: CONIN$'; then
    "$ZIP" dv.zip CON >out 2>&1 || true
    grep -qxF 'zip warning: ignoring special file: CON' out ||
        fail "CON: $(cat out)"
fi
mkdir dv
ps "[IO.File]::WriteAllText('\\\\?\\' + (Resolve-Path dv).Path + '\\aux.c', 'a')"
"$ZIP" -qr dv.zip dv
[ "$(list dv.zip)" = "$(printf 'dv/\ndv/aux.c')" ] ||
    fail "file named like a device: $(list dv.zip)"
ps "Remove-Item -LiteralPath ('\\\\?\\' + (Resolve-Path dv).Path) -Recurse -Force"

# Unicode names: from stdin as UTF-8, and from recursion (bit 11 set,
# checked by .NET decoding the name)
printf 'tree/caf\303\251.txt\r\n' | "$ZIP" -q at.zip -@
rm -rf x4
ps "Expand-Archive -Path at.zip -DestinationPath x4"
ps "if (!(Test-Path -LiteralPath ('x4\tree\caf' + [char]0xe9 + '.txt'))) { exit 1 }" ||
    fail "-@ UTF-8 name"

# ...and typed at a console, read as UTF-16, as output is written to
# one, not in its code page (437 here, where e-acute is 0x82 and the
# Cyrillic a is a ?, a wildcard). As with ReadFile, a line that starts
# with Ctrl+Z ends the input. Typed into a pseudo console, where there is
# one (Windows 10 1809 and later).
cp "$testdir/pty.cs" .
mkdir typed
ps "Set-Content -LiteralPath ('typed\caf' + [char]0xe9 + '.txt') -Value e
    Set-Content -LiteralPath ('typed\' + [char]0x430 + '.txt') -Value a
    Set-Content -LiteralPath ('typed\' + [char]0xd83d + [char]0xde00) -Value s
    Set-Content -LiteralPath typed\b.txt -Value b"
st=$(ps "Add-Type -TypeDefinition (Get-Content -Raw pty.cs)
         \$cr = [string][char]13
         [Pty]::Run('\"$ZIP\" -q typed.zip -@', @(
             ('typed\caf' + [char]0xe9 + '.txt' + \$cr),
             ('typed\' + [char]0x430 + '.txt' + \$cr),
             ('typed\' + [char]0xd83d + [char]0xde00 + \$cr),
             ([string][char]26 + \$cr), ('typed\b.txt' + \$cr)))" |
     tr -d '\r')
entries() {  # archive: its names, as .NET decodes them, compared to want
    ps "Add-Type -AssemblyName System.IO.Compression.FileSystem
        \$z = [IO.Compression.ZipFile]::OpenRead((Resolve-Path '$1').Path)
        \$n = (\$z.Entries | ForEach-Object { \$_.FullName }) -join ' '
        \$z.Dispose()
        if (\$n -eq ($2)) { 'ok' } else { \$n }" | tr -d '\r'
}
if [ "$st" != -2 ]; then
    [ "$st" = 0 ] || fail "-@ typed at a console: $st"
    got=$(entries typed.zip "'typed/caf' + [char]0xe9 + '.txt typed/' +
                             [char]0x430 + '.txt typed/' +
                             [char]0xd83d + [char]0xde00")
    [ "$got" = ok ] || fail "-@ typed at a console: entries $got"

    # So is the console opened by name, here for -i patterns
    st=$(ps "Add-Type -TypeDefinition (Get-Content -Raw pty.cs)
             \$cr = [string][char]13
             [Pty]::Run('\"$ZIP\" -qr typed2.zip typed -i @CON', @(
                 ('*caf' + [char]0xe9 + '*' + \$cr),
                 ([string][char]26 + \$cr)))" | tr -d '\r')
    [ "$st" = 0 ] || fail "-i @CON typed at a console: $st"
    got=$(entries typed2.zip "'typed/caf' + [char]0xe9 + '.txt'")
    [ "$got" = ok ] || fail "-i @CON typed at a console: entries $got"
fi

# Names in the OEM code page, flag bit 11 clear and no Unicode path
# field, as Explorer's zip folder writes them, are decoded to match
# files, as Info-ZIP's port does (0x82 is e-acute in code pages 437 and
# 850); the entry is then written under the decoded name
oem() {  # archive: an empty caf\x82.txt from 2020, made on FAT
    { printf 'PK\003\004\012\0\0\0\0\0\0\0\041P\0\0\0\0\0\0\0\0\0\0\0\0'
      printf '\010\0\0\0caf\202.txt'
      printf 'PK\001\002\024\0\012\0\0\0\0\0\0\0\041P\0\0\0\0\0\0\0\0\0\0\0\0'
      printf '\010\0\0\0\0\0\0\0\0\0\040\0\0\0\0\0\0\0caf\202.txt'
      printf 'PK\005\006\0\0\0\0\001\0\001\0\066\0\0\0\046\0\0\0\0\0'; } >"$1"
}
ps "Set-Content -LiteralPath ('caf' + [char]0xe9 + '.txt') -Value new -NoNewline"
u=$(printf 'caf\303\251.txt')
oem o1.zip
printf '%s\r\n' "$u" | "$ZIP" o1.zip -@ >out
grep -q "^updating: $u" out || fail "OEM name: $(cat out)"
[ "$(list o1.zip | wc -l)" = 1 ] || fail "OEM name duplicated"
oem o2.zip
"$ZIP" -f o2.zip >out
grep -q "^freshening: $u" out || fail "-f OEM name: $(cat out)"
oem o3.zip
printf '%s\r\n' "$u" | "$ZIP" -d o3.zip -@ >out 2>&1
grep -q "^deleting: $u" out || fail "-d OEM name: $(cat out)"
oem o4.zip
"$ZIP" -d o4.zip 'caf?.txt' >out 2>&1
grep -q '^deleting: caf' out || fail "-d OEM name pattern: $(cat out)"

# Other names that are not UTF-8 are in the ANSI code page, the port's
# own, as PKZIP for Windows 2.5 (known by attributes beyond DOS's) and
# Unix tools in Latin-1 store them (0xe9 is e-acute in code page 1252)
ansi() {  # archive made-by attributes: an empty caf\xe9.txt from 2020
    { printf 'PK\003\004\012\0\0\0\0\0\0\0\041P\0\0\0\0\0\0\0\0\0\0\0\0'
      printf '\010\0\0\0caf\351.txt'
      printf "PK\\001\\002$2\\012\\0\\0\\0\\0\\0\\0\\0\\041P"
      printf '\0\0\0\0\0\0\0\0\0\0\0\0\010\0\0\0\0\0\0\0\0\0'
      printf "$3\\0\\0\\0\\0caf\\351.txt"
      printf 'PK\005\006\0\0\0\0\001\0\001\0\066\0\0\0\046\0\0\0\0\0'; } >"$1"
}
k='HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage'
acp=$(ps "(Get-ItemProperty '$k').ACP" | tr -d '\r')
if [ "$acp" = 1252 ]; then
    ansi a1.zip '\031\0' '\040\0\244\201'  # PKZIP 2.5 for Windows
    printf '%s\r\n' "$u" | "$ZIP" a1.zip -@ >out
    grep -q "^updating: $u" out || fail "ANSI name: $(cat out)"
    [ "$(list a1.zip | wc -l)" = 1 ] || fail "ANSI name duplicated"
    ansi a2.zip '\036\003' '\0\0\244\201'  # Unix
    "$ZIP" -f a2.zip >out
    grep -q "^freshening: $u" out || fail "-f ANSI name: $(cat out)"
    ansi a3.zip '\031\0' '\040\0\244\201'
    "$ZIP" a3.zip 'caf*' >out
    grep -q "^updating: $u" out || fail "ANSI name, wildcard: $(cat out)"
    [ "$(list a3.zip | wc -l)" = 1 ] || fail "ANSI name duplicated (wildcard)"
fi
rm caf*.txt

# ...but one so long, decoded (here 0xc4, U+2500, three bytes as UTF-8,
# 22,000 times), that no header could hold it, is skipped as a long path
# is, keeping the entry, rather than written with its length wrapped
cat >oem5.ps1 <<'EOF'
$part = [string]::new([char]0x2500, 250)
$rel = (@($part) * 88) -join '\'
$base = '\\?\' + (Resolve-Path .).Path + '\oem5'
New-Item -ItemType Directory -Path ($base + '\' + $rel) | Out-Null
Set-Content -LiteralPath ($base + '\' + $rel + '\f.txt') -Value new -NoNewline
$name = [Collections.Generic.List[byte]]::new()
foreach ($i in 1..88) { $name.AddRange([byte[]](@(0xC4) * 250)); $name.Add(0x2F) }
$name.AddRange([Text.Encoding]::ASCII.GetBytes('f.txt'))
$n = $name.ToArray()
$ms = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter($ms)
$w.Write([uint32]0x04034b50); $w.Write([uint16]10); $w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint16]0x1421)
$w.Write([uint32]0); $w.Write([uint32]0); $w.Write([uint32]0); $w.Write([uint16]$n.Length); $w.Write([uint16]0); $w.Write($n)
$cd = $ms.Position
$w.Write([uint32]0x02014b50); $w.Write([uint16]0x14); $w.Write([uint16]10); $w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint16]0x1421)
$w.Write([uint32]0); $w.Write([uint32]0); $w.Write([uint32]0); $w.Write([uint16]$n.Length); $w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint16]0)
$w.Write([uint32]0x20); $w.Write([uint32]0); $w.Write($n)
$cs = $ms.Position - $cd
$w.Write([uint32]0x06054b50); $w.Write([uint16]0); $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]1); $w.Write([uint32]$cs); $w.Write([uint32]$cd); $w.Write([uint16]0)
$w.Flush()
[IO.File]::WriteAllBytes((Resolve-Path .).Path + '\o5.zip', $ms.ToArray())
EOF
powershell -NoProfile -NonInteractive -Command - <oem5.ps1 >/dev/null
cp o5.zip o5.orig
set +e
(cd oem5 && "$ZIP" -u ../o5.zip >../out 2>../err)
st=$?
set -e
ps "Remove-Item -LiteralPath ('\\\\?\\' + (Resolve-Path oem5).Path) -Recurse -Force"
[ $st = 18 ] && grep -q '^zip warning: name too long for a zip entry: ' err ||
    fail "OEM name too long: $st $(head -c 200 err)"
cmp -s o5.zip o5.orig || fail "OEM name too long: archive changed"

# Long paths beyond MAX_PATH
long=$(printf '%0100d' 0 | tr 0 d)/$(printf '%0100d' 0 | tr 0 e)/$(printf '%0100d' 0 | tr 0 f)
ps "\$p = '\\\\?\\' + (Resolve-Path .).Path + '\\deep\\$long' -replace '/', '\\';
    New-Item -ItemType Directory -Path \$p | Out-Null;
    Set-Content -LiteralPath (\$p + '\\file.txt') -Value deep -NoNewline"
"$ZIP" -qr deep.zip deep
list deep.zip | grep -q "deep/$long/file.txt" || fail "long path"
"$ZIP" -q deep2.zip "//./$here/deep/$long/file.txt" || fail "long //./ path"
"$ZIP" -q "//./$here/deep/$long/deep3.zip" deep2.zip ||
    fail "long //./ archive path"
ps "Remove-Item -LiteralPath ('\\\\?\\' + (Resolve-Path deep).Path) -Recurse -Force"

# Merging replaces the archive in place
"$ZIP" -q m.zip tree/a.txt tree/one
"$ZIP" -q m.zip tree/b.txt tree/a.txt
list m.zip >got
printf 'tree/a.txt\ntree/one\ntree/b.txt\n' >want.txt
cmp -s got want.txt || fail "merge: $(cat got)"
"$ZIP" -qd m.zip tree/one
list m.zip | grep -q one && fail "-d"
ls | grep -q '^zi[0-9]' && fail "temporary file left behind"

# Links at the archive path survive: the archive is replaced where they
# lead, and a dangling link gets its target created (making links needs
# Developer Mode or elevation)
mkdir -p al/dist al/store
"$ZIP" -q al/store/r.zip tree/a.txt
if cmd /c 'mklink al\dist\rel.zip ..\store\r.zip' >/dev/null 2>&1; then
    "$ZIP" -q al/dist/rel.zip tree/b.txt
    [ -L al/dist/rel.zip ] || fail "archive link replaced"
    [ "$(list al/store/r.zip | tr '\n' ' ')" = "tree/a.txt tree/b.txt " ] ||
        fail "archive through a link: $(list al/store/r.zip)"
    cmd /c 'mklink al\dist\dangling.zip ..\store\new.zip' >/dev/null
    "$ZIP" -q al/dist/dangling.zip tree/one
    [ -L al/dist/dangling.zip ] || fail "dangling archive link replaced"
    [ "$(list al/store/new.zip)" = tree/one ] ||
        fail "dangling archive link: $(list al/store/new.zip)"
fi
ls al/dist al/store | grep -q '^zi[0-9]' &&
    fail "temporary file left beside an archive link"

# Through a link, a temporary file that cannot be created is named as
# the archive was, from its directory if the target is there, else by
# a plain drive path (not \\?\)
mkdir tf tfo
"$ZIP" -q tf/real.zip tree/a.txt
"$ZIP" -q tfo/real.zip tree/a.txt
if cmd /c 'mklink tf\in.zip real.zip' >/dev/null 2>&1; then
    cmd /c 'mklink tf\out.zip ..\tfo\real.zip' >/dev/null
    icacls tf /deny "$USERNAME:(WD)" >/dev/null
    icacls tfo /deny "$USERNAME:(WD)" >/dev/null
    set +e
    "$ZIP" tf/in.zip tree/b.txt 2>err1; st1=$?
    "$ZIP" tf/out.zip tree/b.txt 2>err2; st2=$?
    set -e
    icacls tf /remove:d "$USERNAME" >/dev/null
    icacls tfo /remove:d "$USERNAME" >/dev/null
    [ $st1 = 10 ] && grep -q 'Temporary file failure (tf/zi[0-9]*)' err1 ||
        fail "linked archive temp name: $st1 $(cat err1)"
    [ $st2 = 10 ] &&
        grep -q 'Temporary file failure (.:\\.*\\tfo\\zi[0-9]*)' err2 ||
        fail "linked archive temp name elsewhere: $st2 $(cat err2)"
    "$ZIP" -q tf/in.zip tree/b.txt
    [ -L tf/in.zip ] && [ "$(list tf/real.zip | wc -l)" = 2 ] ||
        fail "archive through a link in its directory"
fi

# A read-only archive is refused (15) before any work, and left alone
"$ZIP" -q ro.zip tree/a.txt
cp ro.zip ro.orig
attrib +r ro.zip
"$ZIP" ro.zip tree/b.txt >out 2>err && fail "read-only archive updated"
grep -q 'Could not create output file (ro.zip)' err ||
    fail "read-only archive: $(cat err)"
grep -q adding out && fail "read-only archive: work done first"
expect_status 15 "$ZIP" -d ro.zip tree/a.txt
cmp -s ro.zip ro.orig || fail "read-only archive changed"
attrib -r ro.zip

# So is one that another process holds open without sharing delete
# access, which replacing it needs, or without sharing reading
for share in ReadWrite None; do
    rm -f held done
    ps "\$f = [IO.File]::Open('ro.zip', 'Open', 'Read', '$share');
        Set-Content held '';
        for (\$i = 0; \$i -lt 600 -and !(Test-Path done); \$i++) {
            Start-Sleep -Milliseconds 50
        }
        \$f.Close()" &
    pid=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -e held ] && break; sleep 1; done
    set +e
    "$ZIP" ro.zip tree/b.txt >out 2>err; st=$?
    set -e
    : >done
    wait $pid || true
    [ $st = 15 ] && grep -q 'Could not create output file (ro.zip)' err ||
        fail "archive held ($share): $st $(cat err)"
    grep -q adding out && fail "archive held ($share): work done first"
    cmp -s ro.zip ro.orig || fail "archive held ($share) changed"
done

# So is an archive name that cannot be examined, for any reason but
# nothing there, as Info-ZIP's port fails to create it then, the reason
# worded by its C runtime for the errno to which it maps the error: a
# share that does not exist is no such file, and a name that no file can
# have, or a link to itself, which the system cannot follow, an invalid
# argument
long=$(printf 'n%.0s' $(seq 260)).zip
for a in //localhost/nosuchshare/x.zip 'a<b.zip' "$long" loop.zip; do
    why='Invalid argument'
    case $a in
    //*) why='No such file or directory';;
    loop.zip) cmd /c 'mklink loop.zip loop.zip' >/dev/null 2>&1 || continue;;
    esac
    set +e
    "$ZIP" "$a" tree/a.txt >out 2>err; st=$?
    set -e
    [ $st = 15 ] && grep -qx "zip I/O error: $why" err &&
        grep -qx "zip error: Could not create output file ($a)" err ||
        fail "unexaminable archive $a: $st $(cat err)"
    grep -q adding out && fail "unexaminable archive $a: work done first"
done
rm -f loop.zip

# ...as is one that another process holds delete-pending
cat >dp.cs <<'EOF'
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
public static class Pending {
    [DllImport("kernel32.dll")]
    static extern bool SetFileInformationByHandle(System.IntPtr h, int c,
                                                  ref byte discard, int n);
    public static FileStream Hold(string path) {
        FileStream f = new FileStream(path, FileMode.Open,
            FileSystemRights.Read | FileSystemRights.Delete,
            FileShare.ReadWrite | FileShare.Delete, 1, FileOptions.None);
        byte discard = 1;
        SetFileInformationByHandle(f.SafeFileHandle.DangerousGetHandle(),
                                   4, ref discard, 1);  // FileDispositionInfo
        return f;
    }
}
EOF
"$ZIP" -q dp.zip tree/a.txt
rm -f held done
ps "Add-Type -TypeDefinition (Get-Content -Raw dp.cs);
    \$f = [Pending]::Hold((Resolve-Path dp.zip).Path);
    Set-Content held '';
    for (\$i = 0; \$i -lt 600 -and !(Test-Path done); \$i++) {
        Start-Sleep -Milliseconds 50
    }
    \$f.Close()" &
pid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -e held ] && break; sleep 1; done
set +e
"$ZIP" dp.zip tree/b.txt >out 2>err; st=$?
set -e
: >done
wait $pid || true
[ $st = 15 ] && grep -qx 'zip I/O error: Permission denied' err ||
    fail "delete-pending archive: $st $(cat err)"
grep -q adding out && fail "delete-pending archive: work done first"
[ ! -e dp.zip ] || fail "delete-pending archive not deleted"

# A replaced archive keeps its hidden, system, and not-indexed
# attributes, as POSIX keeps the mode, with the archive bit set
"$ZIP" -q ha.zip tree/a.txt
attrib +h +s +i -a ha.zip
"$ZIP" -q ha.zip tree/b.txt
[ "$(list ha.zip | wc -l)" = 2 ] || fail "hidden archive not updated"
got=$(ps "(Get-Item -Force ha.zip).Attributes" | tr -d '\r')
[ "$got" = "Hidden, System, Archive, NotContentIndexed" ] ||
    fail "replaced archive attributes: $got"

# Replacing an archive that another process holds open with delete
# sharing, as scanners and indexers do
ps "\$f = [IO.File]::Open('m.zip', 'Open', 'Read', 'ReadWrite, Delete');
    & '$ZIP' -q m.zip tree/one; \$s = \$LASTEXITCODE; \$f.Close();
    exit \$s" || fail "replacing an archive held open"
list m.zip | grep -q one || fail "archive held open: contents"

# Concurrent runs in one directory: each skips the temporary file that
# the other holds delete-pending
head -c 30000000 /dev/urandom >big
"$ZIP" -q9 c1.zip big & p1=$!
"$ZIP" -q9 c2.zip big & p2=$!
wait $p1 || fail "concurrent run 1"
wait $p2 || fail "concurrent run 2"
ls | grep -q '^zi[0-9]' && fail "temporary file left behind (concurrent)"
rm big

# Determinism
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX9r d1.zip tree
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX9r d2.zip tree
cmp -s d1.zip d2.zip || fail "not deterministic"
expect_status 16 env SOURCE_DATE_EPOCH= "$ZIP" -q ep.zip tree/a.txt

# DOS times are local by each year's own daylight saving rules, as
# Windows and .NET give them. Departure: Info-ZIP's port, by its C
# runtime, applies this year's to every year (in the US, its DOS times
# for March and late October 2006 are an hour later).
mkdir dst
for t in 2006-03-15T16:00:00Z 2006-11-01T16:00:00Z 2026-01-15T16:00:00Z \
         2026-07-01T16:00:00Z; do  # in order, as entries are
    f=dst/${t%%T*}.txt
    printf d >"$f"
    ps "(Get-Item '$f').LastWriteTimeUtc =
            [DateTime]::Parse('$t').ToUniversalTime();
        '${f#dst/} ' + (Get-Item '$f').LastWriteTime.ToString('s')"
done | tr -d '\r' >want.txt
"$ZIP" -qXj dst.zip dst/*.txt
ps "Add-Type -AssemblyName System.IO.Compression.FileSystem;
    \$z = [IO.Compression.ZipFile]::OpenRead((Resolve-Path dst.zip).Path);
    foreach (\$e in \$z.Entries) {
        \$e.Name + ' ' + \$e.LastWriteTime.ToString('s')
    }
    \$z.Dispose()" | tr -d '\r' >got
cmp -s got want.txt || fail "DOS times: $(cat got), not $(cat want.txt)"

expect_status 12 "$ZIP" none.zip missing
expect_status 16 "$ZIP" -e bad.zip tree/a.txt

# Info-ZIP's Windows port has no -y (links are followed)
expect_status 16 "$ZIP" -y bad.zip tree/a.txt
expect_status 16 "$ZIP" --symlinks bad.zip tree/a.txt
expect_status 16 "$ZIP" -S- bad.zip tree/a.txt
"$ZIP" --sym bad.zip tree/a.txt 2>err && fail "--sym succeeded"
grep -q "(long option 'sym' not supported)$" err || fail "--sym: $(cat err)"

# The port's own long options, unsupported here, are names that
# abbreviations must tell apart: --i is include or ignore-case
"$ZIP" li.zip tree/a.txt --i tree/a.txt 2>err && fail "--i succeeded"
grep -q "(long option 'i' ambiguous)$" err || fail "--i: $(cat err)"
"$ZIP" --ig bad.zip tree/a.txt 2>err && fail "--ig succeeded"
grep -q "(long option 'ignore-case' not supported)$" err ||
    fail "--ig: $(cat err)"
for opt in --archive-clear --archive-set --use-privileges -AC -AS -ic; do
    expect_status 16 "$ZIP" $opt bad.zip tree/a.txt
done

# Options from ZIPOPT, where as in Info-ZIP's port a double-quoted word
# keeps its spaces and backslashes
printf s >'tree/a b.txt'
env ZIPOPT='-q -x "tree\a b.txt" tree\b.txt' "$ZIP" -r env.zip tree >out
[ ! -s out ] || fail "ZIPOPT=-q: $(cat out)"
list env.zip | grep -q 'tree/a b.txt' && fail "ZIPOPT quoted pattern"
list env.zip | grep -q tree/b.txt && fail "ZIPOPT pattern"
list env.zip | grep -q tree/a.txt || fail "ZIPOPT patterns: $(list env.zip)"
rm 'tree/a b.txt'

[ ! -e "$tmp/FAILED" ] || exit 1
echo "windows zip tests pass"
