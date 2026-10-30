#!/usr/bin/env python3
"""Pack a built frontend (www/) into the single blob embedded in the app image.

Why a blob rather than one EMBED_FILES entry per file: vite content-hashes its
output (`assets/index-DWtLor9m.js`), so the file list is not knowable when CMake
configures -- but EMBED_FILES needs it exactly then. One blob with a fixed name
makes the CMake input a constant, and the frontend is free to split chunks, add
fonts or icons, and rename everything on each build without touching C++ or
CMake.

The blob's directory doubles as the manifest a fetch protocol needs, so it is
not built twice: name, stored size, gzip flag and a content hash per file.

Layout (little-endian, matching WebAssets.cpp):

    header      32 bytes   magic, version, entry count, offsets, bundle hash
    entries     120 bytes each, sorted by name
    payloads    stored bytes, in entry order

Entries are fixed-size so the reader is pointer arithmetic with no name pool to
validate. That caps a name length; the build fails loudly rather than
truncating, since a silently missing asset is a blank page.
"""

import argparse
import gzip
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = b"SXWA"
VERSION = 1

HEADER_FORMAT = "<4sHHIII8s4x"      # 32 bytes
ENTRY_FORMAT = "<96sIII8s4x"        # 120 bytes
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
ENTRY_SIZE = struct.calcsize(ENTRY_FORMAT)

# One byte of the name field is reserved for the NUL terminator, so WebFile::name
# can point straight into the blob. 96 is not arbitrary: vite's hashed font names
# already reach 52 bytes, and a build that dies on a renamed dependency is a bad
# way to find out the field was sized to today's bundle.
MAX_NAME = 95
MAX_ENTRIES = 0xFFFF

FLAG_GZIP = 1 << 0

# Already-compressed payloads. Gzipping one costs the browser a decompress pass
# and buys nothing (or loses, when the container grows).
PRECOMPRESSED = {".woff2", ".woff", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".gz", ".zip"}

PLACEHOLDER = (
    b"<!doctype html><html><body><h1>Frontend not built</h1>"
    b"<p>Install <a href=\"https://pnpm.io\">pnpm</a> and rebuild, or run: "
    b"<code>cd frontend &amp;&amp; pnpm install &amp;&amp; pnpm build</code></p>"
    b"</body></html>\n"
)


def collect(src: Path):
    """Every file under src, as (name, raw bytes), sorted for a reproducible blob."""
    if not src.is_dir():
        return []
    files = []
    for path in sorted(p for p in src.rglob("*") if p.is_file()):
        name = path.relative_to(src).as_posix()
        files.append((name, path.read_bytes()))
    return files


def store(name: str, raw: bytes):
    """Compress only when it actually pays. Fonts, PNGs and other already-packed
    assets grow under gzip, and the per-file flag means the consumer never has to
    guess which is which.

    A size comparison alone is not enough: Inter's latin subset gzips 7 bytes
    SMALLER than it started, so "smaller" stored it compressed and every browser
    then paid a full decompress pass over 48 KB to save seven bytes. Formats that
    carry their own compression are skipped by extension, and the comparison
    stays for everything else."""
    if Path(name).suffix.lower() in PRECOMPRESSED:
        return raw, 0
    packed = gzip.compress(raw, 9, mtime=0)   # mtime=0: same input, same bytes
    if len(packed) < len(raw):
        return packed, FLAG_GZIP
    return raw, 0


def build(files):
    entries = []
    payloads = []
    data_offset = HEADER_SIZE + len(files) * ENTRY_SIZE
    cursor = data_offset

    for name, raw in files:
        encoded = name.encode("utf-8")
        if len(encoded) > MAX_NAME:
            sys.exit(f"web_assets: name too long ({len(encoded)} > {MAX_NAME} bytes): {name}")

        blob, flags = store(name, raw)
        digest = hashlib.sha256(blob).digest()[:8]
        entries.append(struct.pack(ENTRY_FORMAT, encoded, cursor, len(blob), flags, digest))
        payloads.append(blob)
        cursor += len(blob)

        kind = "gz" if flags & FLAG_GZIP else "raw"
        pct = (1 - len(blob) / len(raw)) * 100 if raw else 0
        print(f"  {name:<44} {len(raw):>8} -> {len(blob):>8} ({kind}, -{pct:.0f}%)")

    # One hash over names and stored bytes: identifies a whole bundle in a single
    # comparison, so a cache can decide to skip every file without walking them.
    bundle = hashlib.sha256()
    for entry, payload in zip(entries, payloads):
        bundle.update(entry[:MAX_NAME + 1])
        bundle.update(payload)

    header = struct.pack(
        HEADER_FORMAT, MAGIC, VERSION, len(files),
        HEADER_SIZE, data_offset, cursor, bundle.digest()[:8],
    )
    return header + b"".join(entries) + b"".join(payloads)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", required=True, type=Path, help="directory holding the built frontend")
    parser.add_argument("--out", required=True, type=Path, help="blob to write")
    args = parser.parse_args()

    files = collect(args.src)
    if not files:
        # A device must build without node installed, and a bricked-looking build
        # is worse than an honest placeholder page. The blob is always valid, so
        # the firmware never has to handle a missing one.
        print(f"web_assets: nothing in {args.src} - embedding a placeholder page")
        files = [("index.html", PLACEHOLDER)]

    if len(files) > MAX_ENTRIES:
        sys.exit(f"web_assets: too many files ({len(files)} > {MAX_ENTRIES})")

    blob = build(files)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f"web_assets: {len(files)} file(s), {len(blob)} bytes -> {args.out}")


if __name__ == "__main__":
    main()
