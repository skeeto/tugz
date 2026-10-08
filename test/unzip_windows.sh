#!/bin/sh
# Windows end-to-end tests of unzip.exe, run under w64devkit: names in
# the OEM and ANSI code pages and in Unicode, names mapped for Windows,
# attributes and times, links in the destination, long paths, the
# console prompt, wildcard archive names, and exit statuses. Archives
# made by Explorer's zip folder and by tugz's zip are extracted, and
# compared with what Windows' own readers extract: Explorer, .NET's
# Expand-Archive, and tar (libarchive).
# Usage: TUGZ_ZIP=./zip.exe sh test/unzip_windows.sh ./unzip.exe
set -e

unset UNZIP UNZIPOPT ZIPOPT ZIP  # options for unzip and zip
[ -n "$TUGZ_ZIP" ] || { echo "TUGZ_ZIP must name tugz's zip" >&2; exit 1; }
UNZIP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ZIP=$(cd "$(dirname "$TUGZ_ZIP")" && pwd)/$(basename "$TUGZ_ZIP")
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
    "$@" >/dev/null 2>&1 </dev/null
    got=$?
    set -e
    [ "$got" = "$want" ] || fail "expected status $want, got $got: $*"
}

ps() {
    powershell -NoProfile -NonInteractive -Command "$1"
}

# Whether a file is there, given a PowerShell expression for its name
# relative to the current directory, such as one with [char] codes,
# looked for as a \\?\ path
exists() {
    ps "if (!(Test-Path -LiteralPath ('\\\\?\\' + (Get-Location).Path +
                                      '\\' + ($1)))) { exit 1 }"
}

# A file's attributes and its times, local, as .NET gives them
attrs() {
    ps "(Get-Item -Force -LiteralPath '$1').Attributes" | tr -d '\r'
}
mtime() {
    ps "(Get-Item -Force -LiteralPath '$1').LastWriteTime.ToString('s')" |
        tr -d '\r'
}
mtimeutc() {
    ps "(Get-Item -Force -LiteralPath '$1').LastWriteTimeUtc.ToString('s')" |
        tr -d '\r'
}
atimeutc() {
    ps "(Get-Item -Force -LiteralPath '$1').LastAccessTimeUtc.ToString('s')" |
        tr -d '\r'
}

# Extract with Explorer's zip folder, which runs asynchronously.
shell_extract() {  # archive directory count
    mkdir "$2"
    ps "\$s = New-Object -ComObject Shell.Application;
        \$src = \$s.NameSpace((Resolve-Path '$1').Path);
        \$dst = \$s.NameSpace((Resolve-Path '$2').Path);
        \$dst.CopyHere(\$src.Items(), 0x614);
        for (\$i = 0; \$i -lt 100; \$i++) {
            \$n = (Get-ChildItem -Recurse -File -Force '$2').Count;
            if (\$n -ge $3) { break }
            Start-Sleep -Milliseconds 100
        }"
}

# Make an archive with Explorer's zip folder ("Send to, Compressed
# folder") from a directory's contents. It runs asynchronously, in this
# process, holding the archive open, so wait until .NET, sharing it, reads
# the count of files from a complete archive.
shell_compress() {  # archive directory count
    printf 'PK\005\006\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0' >"$1"
    ps "Add-Type -AssemblyName System.IO.Compression
        \$s = New-Object -ComObject Shell.Application
        \$p = (Resolve-Path '$1').Path
        \$dst = \$s.NameSpace(\$p)
        \$dst.CopyHere(\$s.NameSpace((Resolve-Path '$2').Path).Items(), 0x614)
        for (\$i = 0; \$i -lt 300; \$i++) {
            Start-Sleep -Milliseconds 100
            \$f = \$null
            try {
                \$f = [IO.File]::Open(\$p, 'Open', 'Read', 'ReadWrite, Delete')
                \$z = New-Object IO.Compression.ZipArchive(\$f, 'Read')
                \$n = (\$z.Entries | Where-Object { \$_.Name }).Count
                \$z.Dispose()
                if (\$n -ge $3) { exit 0 }
            } catch {
            } finally {
                if (\$f) { \$f.Close() }
            }
        }
        exit 1" || fail "Explorer compressing $2"
}

# Crafted archives, stored entries, entry by entry: names, extra fields,
# and data are printf formats (octal escapes, since shell variables hold
# no NUL), numbers little-endian
f16() {  # as a format
    printf '\\%03o\\%03o' $(($1 & 255)) $(($1>>8 & 255))
}
f32() {
    f16 $(($1 & 65535))
    f16 $(($1>>16 & 65535))
}
le16() {  # as bytes
    printf "$(f16 $1)"
}
le32() {
    printf "$(f32 $1)"
}
# An MS-DOS date and time: year month day hour minute second
dos() {
    echo $(( ($1-1980)<<25 | $2<<21 | $3<<16 | $4<<11 | $5<<5 | $6/2 ))
}
count=0
: >local.bin
: >central.bin
# name flags made-by external-attributes dostime [extra data crc method]
entry() {
    nlen=$(printf "$1" | wc -c)
    elen=$(printf "${6:-}" | wc -c)
    dlen=$(printf "${7:-}" | wc -c)
    off=$(wc -c <local.bin)
    { printf 'PK\003\004'; le16 20; le16 $2; le16 ${9:-0}; le32 $5
      le32 ${8:-0}; le32 $dlen; le32 $dlen; le16 $nlen; le16 $elen
      printf "$1"; printf "${6:-}"; printf "${7:-}"; } >>local.bin
    { printf 'PK\001\002'; le16 $3; le16 20; le16 $2; le16 ${9:-0}; le32 $5
      le32 ${8:-0}; le32 $dlen; le32 $dlen; le16 $nlen; le16 $elen
      le16 0; le16 0; le16 0; le32 $4; le32 $off
      printf "$1"; printf "${6:-}"; } >>central.bin
    count=$((count + 1))
}
finish() {  # archive
    cdoff=$(wc -c <local.bin)
    cdlen=$(wc -c <central.bin)
    { cat local.bin central.bin; printf 'PK\005\006'; le16 0; le16 0
      le16 $count; le16 $count; le32 $cdlen; le32 $cdoff; le16 0; } >"$1"
    count=0
    : >local.bin
    : >central.bin
}
FAT=20        # version 2.0, made on MS-DOS (FAT)
NTFS=2836     # 0x0b14, made on NTFS (Windows NT)
UNIX=798      # 0x031e, made on Unix
FILE=$((0100644 << 16))
d2020=$(dos 2020 1 2 3 4 6)

# Names: in the OEM code page (437 here, where 0x82 is e-acute), flag
# bit 11 clear, made on FAT, as Explorer's zip folder writes them; in
# the ANSI code page (1252 here, where 0xe9 is e-acute), made elsewhere,
# as Unix tools in Latin-1 write them; in UTF-8, flag bit 11 set; and
# by a Unicode path field (0x7075) that checks out, which wins, or one
# that does not (a stale CRC), which is ignored
k='HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage'
oemcp=$(ps "(Get-ItemProperty '$k').OEMCP" | tr -d '\r')
acp=$(ps "(Get-ItemProperty '$k').ACP" | tr -d '\r')
upath() {  # crc utf8-name-format: a 0x7075 field
    n=$(printf "$2" | wc -c)
    printf 'up'; f16 $((n + 5)); printf '\\001'; f32 $1; printf %s "$2"
}
entry 'oem\202.txt' 0 $FAT 32 $d2020
entry 'ansi\351.txt' 0 $UNIX $FILE $d2020
entry 'utf8\303\251.txt' 2048 $UNIX $FILE $d2020
entry 'caf\202.txt' 0 $FAT 32 $d2020 "$(upath 2694278799 '\320\266.txt')"
entry 'n\204me.txt' 0 $FAT 32 $d2020 "$(upath 1 'stale.txt')"
finish names.zip
mkdir names
(cd names && "$UNZIP" -q ../names.zip) || fail "names: status $?"
cd names
if [ "$oemcp" = 437 ] || [ "$oemcp" = 850 ]; then
    exists "'oem' + [char]0xe9 + '.txt'" || fail "OEM name"
    exists "'n' + [char]0xe4 + 'me.txt'" || fail "stale Unicode path"
fi
if [ "$acp" = 1252 ]; then
    exists "'ansi' + [char]0xe9 + '.txt'" || fail "ANSI name"
fi
exists "'utf8' + [char]0xe9 + '.txt'" || fail "UTF-8 name"
exists "[string][char]0x436 + '.txt'" || fail "Unicode path field"
[ "$(ls | wc -l)" = 5 ] || fail "names: $(ls)"
cd ..
# ...and listed, through the console's or pipe's UTF-8, likewise
"$UNZIP" -l names.zip >out
grep -q "utf8$(printf '\303\251').txt" out || fail "-l UTF-8 name"
grep -q "$(printf '\320\266').txt" out || fail "-l Unicode path field"
[ "$oemcp" != 437 ] || grep -q "oem$(printf '\303\251').txt" out ||
    fail "-l OEM name: $(cat out)"

# Names mapped for Windows, as Info-ZIP's port maps them: device names
# get a '_' before them (with any extension), the characters Windows
# refuses become '_', trailing dots and spaces are dropped (unlike the
# port, which leaves that to the system), and names made on FAT use '\'
entry 'aux.txt' 0 $UNIX $FILE $d2020
entry 'CON' 0 $UNIX $FILE $d2020
entry 'com1.tar.gz' 0 $UNIX $FILE $d2020
entry 'conin$' 0 $UNIX $FILE $d2020
entry 'a:b.txt' 0 $UNIX $FILE $d2020
entry 'x<y>z|w"q?r*s.txt' 0 $UNIX $FILE $d2020
entry 'trail. . ' 0 $UNIX $FILE $d2020
entry 'dir. /f.txt' 0 $UNIX $FILE $d2020
entry 'nul/g.txt' 0 $UNIX $FILE $d2020
entry 'fat\\sub\\f.txt' 0 $FAT 32 $d2020
finish map.zip
mkdir map
set +e
(cd map && "$UNZIP" ../map.zip >../out 2>../err)
st=$?
set -e
[ $st = 1 ] || fail "mapped names: status $st: $(cat err)"
grep -q 'appears to use backslashes as path separators' err ||
    fail "backslash warning: $(cat err)"
(cd map && find . -type f | sort) >got
printf '%s\n' ./_CON ./_aux.txt ./_com1.tar.gz './_conin$' ./_nul/g.txt \
    ./a_b.txt ./dir/f.txt ./fat/sub/f.txt ./trail ./x_y_z_w_q_r_s.txt \
    | sort >want.txt
cmp -s got want.txt || fail "mapped names: $(cat got)"
ps "if (Get-Item -LiteralPath map\\a_b.txt -Stream * |
        Where-Object { \$_.Stream -ne ':\$DATA' }) { exit 1 }" ||
    fail "a stream was written"

# Attributes, of entries made on FAT or NTFS, whose low byte is the DOS
# attributes: read-only, hidden, and system set, archive set on all
entry 'ro.txt' 0 $FAT 33 $d2020
entry 'hidden.txt' 0 $NTFS 34 $d2020
entry 'system.txt' 0 $FAT 36 $d2020
entry 'plain.txt' 0 $FAT 0 $d2020
entry 'unixro.txt' 0 $UNIX $((0100444 << 16 | 1)) $d2020
finish attr.zip
mkdir attr
(cd attr && "$UNZIP" -q ../attr.zip) || fail "attributes: status $?"
[ "$(attrs attr/ro.txt)" = "ReadOnly, Archive" ] ||
    fail "read-only: $(attrs attr/ro.txt)"
[ "$(attrs attr/hidden.txt)" = "Hidden, Archive" ] ||
    fail "hidden: $(attrs attr/hidden.txt)"
[ "$(attrs attr/system.txt)" = "System, Archive" ] ||
    fail "system: $(attrs attr/system.txt)"
[ "$(attrs attr/plain.txt)" = "Archive" ] ||
    fail "plain: $(attrs attr/plain.txt)"
[ "$(attrs attr/unixro.txt)" = "ReadOnly, Archive" ] ||
    fail "Unix read-only: $(attrs attr/unixro.txt)"

# ...and -o replaces read-only, hidden, and system files, which -n
# leaves alone
expect_status 0 sh -c "cd attr && '$UNZIP' -o ../attr.zip"
[ "$(attrs attr/ro.txt)" = "ReadOnly, Archive" ] || fail "-o read-only"
ps "Set-Content -LiteralPath attr\\plain.txt -Value old -NoNewline"
attrib +r attr\\plain.txt
(cd attr && "$UNZIP" -n ../attr.zip >/dev/null) || fail "-n: status $?"
[ "$(cat attr/plain.txt)" = old ] || fail "-n replaced a file"
(cd attr && "$UNZIP" -o ../attr.zip plain.txt >/dev/null) ||
    fail "-o over a read-only file: status $?"
[ ! -s attr/plain.txt ] || fail "-o over a read-only file: not replaced"
[ "$(attrs attr/plain.txt)" = "Archive" ] ||
    fail "-o over a read-only file: $(attrs attr/plain.txt)"

# Times: MS-DOS times are local by each year's own daylight saving
# rules, as .NET gives them; a local extended timestamp (UT) gives
# modification and access times in UTC, for directories too, set once
# their files are in them; -D skips directories', -DD all of them
ut() {  # flags mtime [atime]: a UT field
    printf 'UT'; f16 $((1 + 4*($# - 1))); printf '\\%03o' $1; f32 $2
    [ $# -lt 3 ] || f32 $3
}
entry 'winter.txt' 0 $FAT 32 $(dos 2006 1 15 12 0 0)
entry 'summer.txt' 0 $FAT 32 $(dos 2006 7 15 12 0 0)
entry 'march.txt' 0 $FAT 32 $(dos 2006 3 20 12 0 0)
entry 'ut.txt' 0 $UNIX $FILE $d2020 "$(ut 3 1000000000 1100000000)"
entry 'd/' 0 $UNIX $((040755 << 16 | 16)) $d2020 "$(ut 1 1200000000)"
entry 'd/f.txt' 0 $UNIX $FILE $d2020
finish times.zip
for D in '' -D -DD; do
    rm -rf times
    mkdir times
    (cd times && "$UNZIP" -q $D ../times.zip) || fail "times $D: status $?"
    if [ "$D" != -DD ]; then
        [ "$(mtime times/winter.txt)" = 2006-01-15T12:00:00 ] ||
            fail "winter DOS time: $(mtime times/winter.txt)"
        [ "$(mtime times/summer.txt)" = 2006-07-15T12:00:00 ] ||
            fail "summer DOS time: $(mtime times/summer.txt)"
        [ "$(mtime times/march.txt)" = 2006-03-20T12:00:00 ] ||
            fail "March DOS time: $(mtime times/march.txt)"
        [ "$(mtimeutc times/ut.txt)" = 2001-09-09T01:46:40 ] ||
            fail "UT time: $(mtimeutc times/ut.txt)"
        [ "$(atimeutc times/ut.txt)" = 2004-11-09T11:33:20 ] ||
            fail "UT access time: $(atimeutc times/ut.txt)"
    else
        [ "$(mtime times/winter.txt)" != 2006-01-15T12:00:00 ] ||
            fail "-DD set a time"
    fi
    if [ -z "$D" ]; then
        [ "$(mtimeutc times/d)" = 2008-01-10T21:20:00 ] ||
            fail "directory time: $(mtimeutc times/d)"
    else
        [ "$(mtimeutc times/d)" != 2008-01-10T21:20:00 ] ||
            fail "$D set a directory time"
    fi
done

# Links, from Unix, become files holding their targets, as in Info-ZIP's
# port, made last
entry 'link' 0 $UNIX $((0120777 << 16)) $d2020 '' 'target' 1181691900
entry 'target' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
finish link.zip
mkdir link
(cd link && "$UNZIP" ../link.zip >../out) || fail "link: status $?"
[ "$(cat link/link)" = target ] || fail "link file: $(cat link/link)"
[ "$(cat link/target)" = hello ] || fail "link's target"
grep -q '^finishing deferred symbolic links:' out || fail "link: $(cat out)"

# Nothing is written through a junction or a directory link in the
# destination: an entry within one is refused, and one named as one
# is not replaced, though the -d directory may be one
mkdir -p jd outside
ps "New-Item -ItemType Junction -Path jd\\j -Target (Resolve-Path outside).Path |
    Out-Null"
entry 'j/evil.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
entry 'j' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
entry 'ok.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
finish evil.zip
set +e
(cd jd && "$UNZIP" -o ../evil.zip >../out 2>../err)
st=$?
set -e
[ $st = 50 ] || fail "junction: status $st: $(cat err)"
grep -q 'checkdir error:  j exists but is not directory' err ||
    fail "junction: $(cat err)"
grep -q 'error:  cannot delete old j' err || fail "junction: $(cat err)"
[ -z "$(ls outside)" ] || fail "written through a junction: $(ls outside)"
[ "$(cat jd/ok.txt)" = hello ] || fail "junction: ok.txt"
expect_status 0 sh -c "cd jd && '$UNZIP' -n ../evil.zip j"
[ -z "$(ls outside)" ] || fail "written through a junction (-n)"
"$UNZIP" -q evil.zip ok.txt -d jd/j || fail "-d junction: status $?"
[ "$(cat outside/ok.txt)" = hello ] || fail "-d junction"
rm outside/ok.txt
if ps "New-Item -ItemType SymbolicLink -Path jd\\s
           -Target (Resolve-Path outside).Path | Out-Null" 2>/dev/null; then
    entry 's/evil.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
    finish evil2.zip
    expect_status 2 sh -c "cd jd && '$UNZIP' -o ../evil2.zip"
    [ -z "$(ls outside)" ] || fail "written through a directory link"
fi

# Long paths, beyond MAX_PATH, in names and in -d
long=$(printf '%0100d' 0 | tr 0 d)/$(printf '%0100d' 0 | tr 0 e)/$(printf '%0100d' 0 | tr 0 f)
entry "$long/file.txt" 0 $UNIX $FILE $d2020 "$(ut 1 1000000000)" \
    'hello\n' 909783072
finish long.zip
mkdir lp
(cd lp && "$UNZIP" -q ../long.zip) || fail "long path: status $?"
exists "'lp\\$long\\file.txt' -replace '/', '\\'" || fail "long path"
"$UNZIP" -q long.zip -d "lp/$long" || fail "long -d: status $?"
exists "'lp\\$long\\$long\\file.txt' -replace '/', '\\'" || fail "long -d"
expect_status 0 "$UNZIP" -qo long.zip -d "lp/$long"
ps "Remove-Item -LiteralPath ('\\\\?\\' + (Resolve-Path lp).Path) -Recurse -Force"

# The prompt: answers read from a pipe, their CRs dropped, then typed at
# a console, read as UTF-16, as output is written to one, so that a new
# name may be any Unicode
entry 'p/a.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
entry 'p/b.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
entry 'p/c.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
entry 'p/d.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
entry 'p/e.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 909783072
finish p.zip
olds() {
    for f in a b c d e; do printf old >p/$f.txt; done
}
"$UNZIP" -q p.zip
olds
printf 'y\r\nn\r\nwhat\r\nr\r\nq.txt\r\nA\r\n' | "$UNZIP" p.zip >out 2>err ||
    fail "prompt: status $?"
[ "$(cat p/a.txt p/b.txt p/c.txt p/d.txt p/e.txt q.txt)" = \
  "$(printf 'hello\noldoldhello\nhello\nhello')" ] || fail "prompt from a pipe"
grep -q 'error:  invalid response \[what\]$' err || fail "prompt: $(cat err)"
rm q.txt
olds
set +e
printf 'n\r\n' | "$UNZIP" p.zip >out 2>err
st=$?
set -e
[ $st = 1 ] && grep -q 'EOF or read error, treating as "\[N\]one"' err ||
    fail "prompt at EOF: $st $(cat err)"

cp "$testdir/pty.cs" .
olds
st=$(ps "Add-Type -TypeDefinition (Get-Content -Raw pty.cs)
         \$cr = [string][char]13
         [Pty]::Run('\"$UNZIP\" p.zip', @(
             ('y' + \$cr), ('n' + \$cr), ('x' + \$cr), ('r' + \$cr),
             ([string][char]0x436 + [char]0xd83d + [char]0xde00 + \$cr),
             ('N' + \$cr)))" | tr -d '\r')
if [ "$st" != -2 ]; then
    [ "$st" = 0 ] || fail "prompt at a console: $st"
    [ "$(cat p/a.txt p/b.txt p/c.txt p/d.txt p/e.txt)" = \
      "$(printf 'hello\noldoldoldold')" ] || fail "prompt at a console"
    exists "[string][char]0x436 + [char]0xd83d + [char]0xde00" ||
        fail "new name typed at a console"
    olds
    st=$(ps "Add-Type -TypeDefinition (Get-Content -Raw pty.cs)
             \$cr = [string][char]13
             [Pty]::Run('\"$UNZIP\" p.zip', @(('A' + \$cr)))" | tr -d '\r')
    [ "$st" = 0 ] || fail "prompt at a console (A): $st"
    [ "$(cat p/a.txt p/b.txt p/c.txt p/d.txt p/e.txt)" = \
      "$(printf 'hello\nhello\nhello\nhello\nhello')" ] ||
        fail "prompt at a console (A)"
fi

# Wildcard archive names, matched as Windows matches names: either
# separator, any case, *.* for names without a period too
mkdir -p wz wout
cp p.zip wz/w1.zip
cp link.zip wz/w2.zip
cp p.zip wz/w3
"$UNZIP" -o 'wz\W?.ZIP' -d wout >out 2>err || fail "wildcard: status $?"
grep -q '^2 archives were successfully processed\.$' err ||
    fail "wildcard: $(cat err)"
[ "$(cat wout/link)" = target ] || fail "wildcard: link.zip"
"$UNZIP" -l 'wz/*.*' >out 2>err || fail "*.*: status $?"
grep -q '^3 archives were successfully processed\.$' err ||
    fail "*.*: $(cat err)"
expect_status 9 "$UNZIP" -l 'wz/none*.zip'
"$UNZIP" -l 'wz/none*.zip' 2>err || true
grep -q '^No zipfiles found\.$' err || fail "no match: $(cat err)"

# ...hidden and system files among them, as UnZip's port lists them
cp p.zip wz/w4.zip
cp p.zip wz/w5.zip
attrib +h wz\\w4.zip
attrib +s wz\\w5.zip
"$UNZIP" -l 'wz/w?.zip' >out 2>err || fail "hidden: status $?"
grep -q '^4 archives were successfully processed\.$' err ||
    fail "hidden: $(cat err)"
grep -q '^Archive:  wz/w4\.zip' out || fail "hidden: $(cat out)"
grep -q '^Archive:  wz/w5\.zip' out || fail "system: $(cat out)"

# Exit statuses
cp p.zip arc.zip
expect_status 0 "$UNZIP" -t arc
expect_status 9 "$UNZIP" -t missing
printf 'not an archive\n' >notzip.txt
expect_status 9 "$UNZIP" -t notzip.txt
expect_status 10 "$UNZIP" -K arc.zip
expect_status 10 "$UNZIP" -X arc.zip
expect_status 11 "$UNZIP" -t arc.zip none.txt
entry 'bad.txt' 0 $UNIX $FILE $d2020 '' 'hello\n' 1
finish badcrc.zip
expect_status 2 "$UNZIP" -t badcrc.zip
mkdir bc
expect_status 2 sh -c "cd bc && '$UNZIP' ../badcrc.zip"
[ ! -e bc/bad.txt ] || fail "bad CRC: file kept"
entry 'd64.bin' 0 $NTFS 32 $d2020 '' 'hello\n' 909783072 9
finish d64.zip
expect_status 81 "$UNZIP" -t d64.zip
mkdir d64
expect_status 81 sh -c "cd d64 && '$UNZIP' ../d64.zip"
[ ! -e d64/d64.bin ] || fail "Deflate64 entry extracted"
head -c 30 p.zip >trunc.zip
expect_status 9 "$UNZIP" -t trunc.zip
printf 'MZ stub\n' | cat - p.zip >sfx.zip
expect_status 1 "$UNZIP" -t sfx.zip

# Ordinary archives, made by tugz's zip, are extracted as Windows' own
# readers extract them: tar, which takes names in the ANSI code page,
# before a name beyond it is added, and an empty directory, which Explorer
# leaves out, after its check
mkdir -p tree/sub/deeper tree/empty
printf 'hello hello hello hello\n' >tree/a.txt
: >tree/zero
head -c 300000 /dev/urandom >tree/sub/random
awk 'BEGIN{for(i=0;i<50000;i++)print i, i*i, "lorem ipsum"}' >tree/sub/text
printf d >tree/sub/deeper/d.txt
ps "Set-Content -LiteralPath ('tree\caf' + [char]0xe9 + '.txt') -Value u -NoNewline"
nfiles=6
for pass in 1 2; do
    rm -rf ours.zip u1 t1 x1 e1
    "$ZIP" -qr ours.zip tree
    mkdir u1 t1 x1
    (cd u1 && "$UNZIP" -q ../ours.zip) || fail "ours.zip: status $?"
    diff -r tree u1/tree >/dev/null || fail "ours.zip: unzip contents"
    if [ $pass = 1 ]; then
        [ -d u1/tree/empty ] || fail "ours.zip: empty directory"
        "$TAR" -xf ours.zip -C t1 || fail "ours.zip: tar"
        diff -r t1/tree u1/tree >/dev/null || fail "ours.zip: tar differs"
    fi
    ps "Expand-Archive -Path ours.zip -DestinationPath x1" ||
        fail "Expand-Archive ours.zip"
    diff -r x1/tree u1/tree >/dev/null || fail "ours.zip: Expand-Archive differs"
    shell_extract ours.zip e1 $nfiles
    diff -r e1/tree u1/tree >/dev/null || fail "ours.zip: Explorer differs"
    rmdir tree/empty 2>/dev/null || true
    ps "Set-Content -LiteralPath ('tree\' + [char]0x436 + '.txt') -Value z -NoNewline"
    nfiles=7
done

# Archives made by Explorer, which writes names in the OEM code page, are
# extracted as Explorer and tar extract them, and as tugz's zip's are.
# .NET Framework's Expand-Archive differs: it decodes such names in the
# ANSI code page. Explorer refuses names beyond the OEM code page.
ps "Remove-Item -LiteralPath ('tree\' + [char]0x436 + '.txt')"
nfiles=6
rm -rf ours.zip u1
"$ZIP" -qr ours.zip tree
mkdir u1 u2 t2
(cd u1 && "$UNZIP" -q ../ours.zip) || fail "ours.zip: status $?"
shell_compress explorer.zip tree $nfiles
(cd u2 && "$UNZIP" -q ../explorer.zip) || fail "explorer.zip: status $?"
diff -r tree u2 >/dev/null || fail "explorer.zip: unzip contents"
diff -r u1/tree u2 >/dev/null || fail "explorer.zip and ours.zip differ"
"$TAR" -xf explorer.zip -C t2 || fail "explorer.zip: tar"
diff -r t2 u2 >/dev/null || fail "explorer.zip: tar differs"
shell_extract explorer.zip e2 $nfiles
diff -r e2 u2 >/dev/null || fail "explorer.zip: Explorer differs"
"$UNZIP" -t explorer.zip >/dev/null || fail "explorer.zip: -t"

[ ! -e "$tmp/FAILED" ] || exit 1
echo "windows unzip tests pass"
