#!/usr/bin/env python3
"""Keep web-flasher images clear of the 1-byte final block that breaks esp-web-tools.

Every esp-web-tools 10.x release up to 10.4.0 bundles an esptool-js whose
flashDeflBlock() builds a debug string from data[0] and data[1] of every
compressed block. A final block of exactly one byte therefore throws "Cannot
read properties of undefined (reading 'toString')" before it is sent: the
install stops at 99 %, the end of the part never reaches the flash, and the
stick reports "No bootable app partitions in the partition table". The 1.4.202
TUL32 image of ip4knx hit this. Fixed in esptool-js 0.6.1 — drop this check
once the flasher pages load an esp-web-tools release that bundles it.

The browser pads each part to a multiple of 4 with 0xFF, deflates it at level 9
(pako 2.1.0 — byte-identical to zlib here) and sends blocks of 16384 bytes.
Every part of a manifest is compressed on its own.

usage:
  webflash_last_block.py [--fix] <image.bin>...
      check the images; with --fix, pad an affected factory image (partition
      table at 0x8000) in place with 0xFF to the next 4 KiB boundary, without
      leaving the partition its last byte is in
  webflash_last_block.py --manifest <manifest.json>...
      check every part of every build, resolved next to the manifest
Exit status 1 if an affected part is left.
"""
import json
import os
import struct
import sys
import zlib

BLOCK = 0x4000          # esptool-js FLASH_WRITE_SIZE with the stub loaded
SECTOR = 0x1000
PART_TABLE = 0x8000


def last_block(data):
    """Size of the final compressed block esptool-js sends for this part."""
    if len(data) % 4:
        data += b'\xff' * (4 - len(data) % 4)
    return len(zlib.compress(data, 9)) % BLOCK or BLOCK


def partition_end(data, offset):
    """End of the partition that holds `offset`, from the table in the image."""
    for i in range(PART_TABLE, min(len(data), PART_TABLE + 0xC00), 32):
        entry = data[i:i + 32]
        if entry[:2] != b'\xaa\x50':
            break
        start, size = struct.unpack('<II', entry[4:12])
        if start <= offset < start + size:
            return start + size
    return None


def padded(data):
    """Return `data`, padded with 0xFF if its final block would be 1 byte.

    Raises ValueError when that cannot be done without touching flash outside
    the partition the image ends in (or the part is no factory image)."""
    if last_block(data) != 1:
        return data
    if data[PART_TABLE:PART_TABLE + 2] != b'\xaa\x50':
        raise ValueError("final web-flash block is 1 byte, and without a partition "
                         "table at 0x8000 it cannot be padded safely")
    limit = partition_end(data, len(data) - 1)
    out = data
    while last_block(out) == 1:
        out += b'\xff' * (SECTOR - len(out) % SECTOR)
    if limit is None or len(out) > limit:
        raise ValueError("final web-flash block is 1 byte, and padding to 0x%x would "
                         "leave the partition the image ends in" % len(out))
    return out


def write_atomic(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fd = os.open(os.path.dirname(os.path.abspath(path)), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    with open(path, 'rb') as f:
        if f.read() != data:
            sys.exit(f"ERROR: read-back of {path} does not match what was written")


def manifest_parts(path):
    with open(path) as f:
        manifest = json.load(f)
    for build in manifest.get('builds', []):
        for part in build.get('parts', []):
            yield build.get('chipFamily', '?'), part['path']


def main(argv):
    fix = '--fix' in argv
    args = [a for a in argv if a not in ('--fix', '--manifest')]
    if '--manifest' in argv:
        files = []
        for m in args:
            for chip, p in manifest_parts(m):
                if p.startswith(('/', 'http:', 'https:', 'data:')):
                    sys.exit(f"ERROR: {m}: part {p} ({chip}) is not a path next to the manifest")
                files.append(os.path.join(os.path.dirname(os.path.abspath(m)), p))
        args = sorted(set(files))
    bad = 0
    for path in args:
        with open(path, 'rb') as f:
            data = f.read()
        name = os.path.basename(path)
        size = last_block(data)
        if size != 1:
            print(f"{name}: final web-flash block {size} bytes — ok")
            continue
        if not fix:
            print(f"ERROR: {name}: final web-flash block is 1 byte — esp-web-tools aborts at 99 %")
            bad += 1
            continue
        try:
            out = padded(data)
        except ValueError as e:
            print(f"ERROR: {name}: {e}")
            bad += 1
            continue
        write_atomic(path, out)
        print(f"{name}: padded 0x{len(data):x} -> 0x{len(out):x} with 0xFF — "
              f"final web-flash block {last_block(out)} bytes")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
