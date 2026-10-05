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

# Determinism
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX9r d1.zip tree
SOURCE_DATE_EPOCH=1700000000 "$ZIP" -qX9r d2.zip tree
cmp -s d1.zip d2.zip || fail "not deterministic"

expect_status 12 "$ZIP" none.zip missing
expect_status 16 "$ZIP" -e bad.zip tree/a.txt

echo "windows zip tests pass"
