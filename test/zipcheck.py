# Verify a zip archive with Python's zipfile, an independent reader, and
# print one line per entry: name, method, flag bits, external attributes,
# and the host that made it.
# Usage: python3 test/zipcheck.py ARCHIVE
import sys
import zipfile

sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
with zipfile.ZipFile(sys.argv[1]) as z:
    bad = z.testzip()
    if bad is not None:
        sys.exit(f"bad entry: {bad}")
    for i in z.infolist():
        print(i.filename, i.compress_type, hex(i.flag_bits),
              hex(i.external_attr), i.create_system)
