# HP Prime firmware container notes

Reverse-engineering notes for the 2026-09-09 update package
(`HP_Prime_Calculator_Firmware_20260909.zip`) and how PrimeShim consumes it.
All offsets are byte offsets in the original files.

## Package contents

| File | Size | Meaning |
|---|---:|---|
| `PRIME_APP.DAT` | 33,554,432 | application image: A: FAT16 volume + OS ELF + A: file manifest |
| `PRIME_MASTER.DAT` | 4,194,304 | boot/master image; magic `EA656XXX.DAT`, version `V0.048.635`; no A:/C: files |
| `PRIME_OS.ROM` | 1,048,576 | 1 MiB boot ROM; FAT strings are part of the boot loader, not a filesystem |
| `version_20260909` | 3 | build tag |

Magic/header of `PRIME_APP.DAT`:

```
00000000  41 50 44 53 4b 58 58 58 2e 44 41 54   APDSKXXX.DAT
00000014  56 31 2e 30 30                        V1.00
```

The first `0x2000` bytes are the container header. Everything from `0x2000`
to EOF is the A: disk image (33,538,048 bytes).

## A: disk — FAT16 at 0x2000

`PRIME_APP.DAT[0x2000:]` is a FAT16 volume:

- boot sector at image offset 0 (DAT `0x2000`), jump `E9 00 00`,
  OEM/volume label `BXC.OS.1`, filesystem type string `FAT16`
- 512-byte sectors, 1 sector per cluster, 2 FATs
- FAT #1 at image offset `0x0200` (DAT `0x2200`); the first entries are
  `FFF8 FFFF FFFF 0005 0006 ...` (contiguous chain)
- root directory and data follow; the A: file checksum manifest is at
  image offset `0x136e000` (DAT `0x1370000`)

The manifest is ASCII, one line per file:

```
<MD5> \APPSLIST.INF
<MD5> \APPSLIST.MAP
<MD5> \BESTABFS.IND
<MD5> \WINDOW\SYSTEM\KRNLLIB.DLL
<MD5> \WINDOW\SYSTEM\MD5DLL.DLL
<MD5> \WINDOW\SYSTEM\SDKLIB.DLL
<MD5> \programs\misc\armfir.elf
<MD5> \programs\misc\diagnose.dat
...
```

The OS ELF lives **inside** the A: volume at `0xE6E00`;
`programs\misc\armfir.elf` is 8,306,652 bytes for this package.  It is the
same ELF that `PrimeShim` extracts as `armfir.elf`.

`fw/prime_fat16.img` is `PRIME_APP.DAT[0x2000:]` already extracted;
`fw/adisk/` is the same volume unpacked as a directory tree (the file MD5s
match the manifest).

## The 8.9 MiB images: `programs\misc\armfir.dat` vs `A:\dummy.DAT`

The A: manifest lists `programs\misc\armfir.dat` (MD5
`600E6A4A5FE890FEDFCAC6083B63318A`), an 8.9 MiB data file **inside** the A:
volume.  It is A: program data, not the file the shim opens at boot.

`A:\dummy.DAT` is a separate C:/user data image created by the device.  It is
**not** in the A: manifest and not in `PRIME_APP.DAT`.  The shim opens it at
process start:

```c
/* src/system_shim.c */
*(uint32_t *)(process_struct + 112) = SDKLIB__OpenFile("A:\\dummy.DAT", "rb");
```

The handle is stored in the guest process structure at `+112`; the app
launcher dereferences data from it.  If `dummy.DAT` belongs to a different
firmware/C: generation, the offsets are stale and opening any app faults at
`+4`.

`runtime/prime_data/A/dummy.DAT` (8,896,512 bytes, MD5
`83b2e647…`) is a capture from the working 15622/15515 install, not something
the update package can recreate.  The 15270 `armfir.dat` (8,897,536) is **not**
a replacement for `dummy.DAT`.

## C: drive

The C: drive is the calculator's user partition.  It is **not** in
`PRIME_APP.DAT`, `PRIME_MASTER.DAT` or `PRIME_OS.ROM`:

- no `&Function.hpapp`, `&Statistics*.hpapp` or `C:\DATA` strings in the
  update package;
- the only `MYSET.INI` occurrence is a string inside the A: volume/ELF, not a
  C: tree;
- `runtime/prime_data/C` is a captured 15270 C: tree, not something the
  update package can regenerate.

So a DAT can supply the OS ELF and the A: volume, but it cannot recreate the
user C: applications.  A C: backup or a matching `prime_data/C` is required
for user apps.

## What PrimeShim builds from an update package

PrimeShim does **not** ship A:/C: data any more.  On import it builds a profile
from the user's own `PRIME_APP.DAT`:

```text
profiles/<sha256(armfir.elf)[:16]>/
  armfir.elf                 extracted and validated by firmware_image.c
  prime_data/A/              FAT16 volume from PRIME_APP.DAT[0x2000:]
    dummy.DAT                hard link/copy of programs/misc/armfir.dat
    WINDOW/SYSTEM/*.DLL
    programs/misc/armfir.dat
  prime_data/C/              empty; firmware initialises it on first run
```

`FirmwarePackage.extractAppDisk()` reads the FAT16 boot sector, both FATs, the
root directory and VFAT long names.  It preserves the name case because the
Linux VFS is case-sensitive.  `APPSLIST.MD5` is verified after extraction.

`dummy.DAT` is not a separate file in the official volume.  `FirmwarePackage`
publishes `programs/misc/armfir.dat` as `A:\dummy.DAT` (hard link when the
filesystem supports it).  That database contains the Prime Sans faces used by
the UI; a blank dummy makes the calculator boot without drawing any text.
`primeshim_ensure_dummy()` repeats the copy as a fallback for profiles created
by older builds.  User apps/programs themselves still require a C: backup; the
official package does not contain them.

## Extraction commands

```sh
# A: FAT16 volume
dd if=PRIME_APP.DAT of=prime_fat16.img bs=1 skip=$((0x2000))

# OS ELF (same offset the native importer uses)
python3 - <<'PY'
data = open('PRIME_APP.DAT','rb').read()
off = data.find(b'\x7fELF')
print(hex(off))
open('armfir.elf','wb').write(data[off:off+8306652])
PY
```

The A: file manifest at `0x1370000` can be verified with `md5sum` against
`fw/adisk`.

## Crash signature

`SIGSEGV`, fault address `0x4`, guest PC in the app-object constructor:
the constructor reads `[r4+8]` (a data pointer) and then `[r0+4]`.  With the
wrong `dummy.DAT` the data pointer is null and the first dereference faults
at `0x4`.  Matching `dummy.DAT` to the firmware generation removes the fault.
