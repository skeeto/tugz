# Verify a zip archive with Python's zipfile, an independent reader, and
# print one line per entry: name, method, flag bits, external attributes,
# the host that made it, internal attributes, the lengths of its central
# and local extra fields, and its comment.
# Usage: python3 test/zipcheck.py ARCHIVE
import struct
import sys
import zipfile

sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
with zipfile.ZipFile(sys.argv[1]) as z:
    bad = z.testzip()
    if bad is not None:
        sys.exit(f"bad entry: {bad}")
    for i in z.infolist():
        z.fp.seek(i.header_offset + 28)
        lextra = struct.unpack("<H", z.fp.read(2))[0]
        print(i.filename, i.compress_type, hex(i.flag_bits),
              hex(i.external_attr), i.create_system, i.internal_attr,
              len(i.extra), lextra, i.comment.decode("utf-8", "replace"))
