#!/usr/bin/env python3
"""Extract the ARM ELF (and locate the app disk) inside an HP Prime update.

The update package ships three opaque blobs:

    PRIME_APP.DAT     32 MiB  "APDSKXXX.DAT" + "V1.00"
    PRIME_MASTER.DAT   4 MiB  "EA656XXX.DAT" + "V0.048.635"
    PRIME_OS.ROM       1 MiB  (no ELF inside)

PRIME_APP.DAT is a small volume: a header, then 320-byte directory records
(`\\.\\NAME.ROM` device path + long file name), then the file data.  The
firmware is a 32-bit ARM ELF stored verbatim, so it can be recovered by
searching for the ELF magic and trusting the ELF header for its length.
No decryption or decompression is involved.

Usage:
    tools/fw_extract.py PRIME_APP.DAT -o out/            # extract armfir.elf
    tools/fw_extract.py PRIME_APP.DAT --list             # show directory
    tools/fw_extract.py PRIME_APP.DAT --info             # header + stats
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
DIR_RECORD = 0x140
APP_MAGIC = b"APDSKXXX.DAT"
MASTER_MAGIC = b"EA656XXX.DAT"
# armfir.dat (the app disk) starts with this 16-byte signature in every image.
DAT_SIGNATURE = b"\xff\xff\xff\xff\x05\x00\x00\x00\x01\x00\x00\x00"


def elf_length(blob, offset):
    """Total size of the ELF starting at offset, from its section headers."""
    header = blob[offset:offset + 52]
    if len(header) < 52:
        return None
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _,
     _, _, e_shentsize, e_shnum, _) = struct.unpack_from("<HHIIIIIHHHHHH", header, 16)
    if e_type != 2 or e_machine != 40:          # ET_EXEC, EM_ARM
        return None
    if e_shnum == 0 or e_shoff == 0:
        return None
    end = e_shoff + e_shnum * e_shentsize
    if end > len(blob):
        return None
    return end, e_entry


def find_elf(blob):
    start = 0
    while True:
        position = blob.find(ELF_MAGIC, start)
        if position < 0:
            return None
        length = elf_length(blob, position)
        if length:
            return position, length[0], length[1]
        start = position + 1


def list_directory(blob):
    entries = []
    marker = b"\\\\.\\"
    position = 0
    while True:
        position = blob.find(marker, position)
        if position < 0:
            break
        record = position - 4
        dos = blob[position:position + 16].split(b"\0")[0].decode("latin1")
        long_name = blob[record + 0x1C:record + 0x29].split(b"\0")[0].decode("latin1")
        entries.append((record, dos, long_name))
        position += len(marker)
    return entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--info", action="store_true")
    args = parser.parse_args()

    blob = args.image.read_bytes()
    print(f"{args.image.name}: {len(blob)} bytes, head={blob[:16]!r}")

    if blob.startswith(APP_MAGIC):
        print("  container: PRIME_APP.DAT (APDSK volume)")
    elif blob.startswith(MASTER_MAGIC):
        print("  container: PRIME_MASTER.DAT (boot/master image)")
        return 0

    if args.list:
        for record, dos, long_name in list_directory(blob):
            print(f"  {record:#08x}  {dos:20s} -> {long_name}")
        return 0

    found = find_elf(blob)
    if not found:
        print("no ARM ELF found", file=sys.stderr)
        return 1
    offset, size, entry = found
    print(f"  ELF at {offset:#x}, {size} bytes ({size / 1048576:.2f} MiB), entry {entry:#010x}")
    dat = blob.find(DAT_SIGNATURE)
    if dat >= 0:
        print(f"  armfir.dat signature at {dat:#x}")

    if args.info:
        print(f"  md5 {hashlib.md5(blob[offset:offset + size]).hexdigest()}")
        return 0

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        target = args.output / "armfir.elf" if args.output.is_dir() or args.output.suffix == "" \
            else args.output
        target.write_bytes(blob[offset:offset + size])
        print(f"  wrote {target} ({size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
