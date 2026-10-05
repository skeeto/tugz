#!/bin/sh
# Windows end-to-end tests of zip.exe, run under w64devkit. Archives are
# extracted by Windows' own readers: Explorer's zip folder (the decoder
# in libdeflate issue #323), .NET's Expand-Archive, and tar (libarchive).
# Also covers wildcards, hidden files, Unicode names, and long paths.
# Usage: sh test/zip_windows.sh ./zip.exe
set -e

ZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
TAR=C:/Windows/System32/tar.exe
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

# Archive names, as tar lists them (with CRLF line endings)
list() {
    "$TAR" -tf "$1" | tr -d '\r'
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
# three-byte string repeats, so its compressed blocks use no distances
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
    "$ZIP" -qX -$level lit.zip tree/literals.txt
    "$TAR" -tf lit.zip >/dev/null || fail "tar -t of literals"

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

# Recursion describes files from the listing, but still examines those
# the archive's size: the archive is left out, a copy of it is not
mkdir self
printf x >self/x
"$ZIP" -qr self/s.zip self
cp self/s.zip self/copy.zip
"$ZIP" -qr self/s.zip self
list self/s.zip | grep -qx self/s.zip && fail "archive added to itself"
list self/s.zip | grep -qx self/copy.zip || fail "same-size file left out"

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
expect_status 12 "$ZIP" -r p5.zip ''

# Names drop a device or UNC prefix along with the drive, as Info-ZIP
"$ZIP" -q p6.zip "//?/$here/tree/one"
[ "$(list p6.zip)" = "${here#?:/}/tree/one" ] ||
    fail "device path name: $(list p6.zip)"

# Only a letter is a drive: "1:s" is stream s of file 1, and named so
ps "Set-Content -LiteralPath 1 -Value f;
    Set-Content -LiteralPath 1 -Stream s -Value s"
"$ZIP" -q p7.zip 1:s
[ "$(list p7.zip)" = 1:s ] || fail "stream name: $(list p7.zip)"

# Unicode names: from stdin as UTF-8, and from recursion (bit 11 set,
# checked by .NET decoding the name)
printf 'tree/caf\303\251.txt\r\n' | "$ZIP" -q at.zip -@
rm -rf x4
ps "Expand-Archive -Path at.zip -DestinationPath x4"
ps "if (!(Test-Path -LiteralPath ('x4\tree\caf' + [char]0xe9 + '.txt'))) { exit 1 }" ||
    fail "-@ UTF-8 name"

# Long paths beyond MAX_PATH
long=$(printf '%0100d' 0 | tr 0 d)/$(printf '%0100d' 0 | tr 0 e)/$(printf '%0100d' 0 | tr 0 f)
ps "\$p = '\\\\?\\' + (Resolve-Path .).Path + '\\deep\\$long' -replace '/', '\\';
    New-Item -ItemType Directory -Path \$p | Out-Null;
    Set-Content -LiteralPath (\$p + '\\file.txt') -Value deep -NoNewline"
"$ZIP" -qr deep.zip deep
list deep.zip | grep -q "deep/$long/file.txt" || fail "long path"
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

expect_status 12 "$ZIP" none.zip missing
expect_status 16 "$ZIP" -e bad.zip tree/a.txt

echo "windows zip tests pass"
