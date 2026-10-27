# HP Prime firmware 2.4.2 (2026-09-09) — container and ELF analysis

Source package: `HP_Prime_Calculator_Firmware_20260909.zip` (11,436,320 bytes).
All numbers below were measured, not inferred; the extractor is
`tools/fw_extract.py`.

## 1. Package contents

| File | Size | Header |
| --- | --- | --- |
| `PRIME_APP.DAT` | 33,554,432 (32 MiB) | `APDSKXXX.DAT` … `V1.00` |
| `PRIME_MASTER.DAT` | 4,194,304 (4 MiB) | `EA656XXX.DAT` … `V0.048.635` |
| `PRIME_OS.ROM` | 1,048,576 (1 MiB) | `20 00 00 30 …` (no ELF) |
| `release_info.txt` | 148,215 | `Date Released: 2026/09/09`, `Firmware Version: 2.4.2` |
| `Prime_FW.md5` | 708 | md5 manifest — **all three blobs verified** |

`EA656` is the HP Prime G1 hardware codename (the same string appears in
`include/ui.h`), so `PRIME_MASTER.DAT` is the boot/master side of the same
machine family.

## 2. `PRIME_APP.DAT` is a small volume, not a compressed blob

Layout as observed:

```text
0x000000   header: "APDSKXXX.DAT\0\0\0\0" "V1.00" then zero padding
0x0d881c   directory: 320-byte records, variable count
              +0x00  u32  0x140 (record size)
              +0x04  char[16]  device path, e.g. "\\.\ARMFIR.ROM"
              +0x1c  char[13]  long name,  e.g. "armfir.elf"
              +0x100 … 0xFFFFFFFF chain terminators (allocation map)
0x0df000   armfir.dat signature (ff ff ff ff 05 00 00 00 01 …)
0x0e6e00   armfir.elf, stored verbatim
```

Directory entries found (7 real files, 4 false positives from data):

| Device path | File |
| --- | --- |
| `\\.\ARMFIR.ROM` | `armfir.elf` |
| `\\.\ARMFIRDB.ROM` | `armfir.dat` |
| `\\.\BESTAFIR.ROM` | `bestafir.exe` |
| `\\.\HELLO.ROM` / `\\.\ARMHELLO.ROM` / `\\.\ARMHELLODB.R` | `hello.exe`, `armhello.elf`, `armhello.dat` |
| `\\.\DIAGNOSE.ROM` | `diagnose.exe` |

Nothing is encrypted or compressed: the ELF sits at a fixed offset and is
self-describing, so extraction only needs the ELF magic plus the section header
count (`tools/fw_extract.py`).

## 3. The firmware ELF

| | 2.4.2 (2026) | 15270 (2025) | Δ |
| --- | --- | --- | --- |
| File size | 8,306,652 (0x7ebfdc) | 8,230,724 (0x7d9744) | +75,928 |
| md5 | `67d4785803375677e4dfe8344f55cc1f` | — | |
| Entry | `0x30604ae8` | `0x30604ae8` | **identical** |
| `ER_RO` | 0x769d84 | 0x7579f0 | +74,644 |
| `ER_RW` | 0x58b0 | 0x669c | −3,884 |
| `ER_ZI` | 0x608c | 0x606c | +32 |
| Type / machine | ET_EXEC / EM_ARM (40) | same | |

The `.comment` section (0x7c7f4 bytes) preserves the full build command line:

```text
ARM Linker, 4.1 [Build 894]
ArmLink --show_cmdline --no_thumb2_library --verbose --map --no_debug
  --ro_base=0x30600000 --cpu=ARM926EJ-S --fpu=SoftVFP --datacompressor=off
  --output=.\obj\armfir.axf --entry=AppMain --userlibpath=../sdk/arm926ej-s
C:\Keil\ARM\RV31\Lib\armlib\c_5.l
```

This confirms, from the firmware itself, the assumptions PrimeShim encodes:
**ARM926EJ-S, ARMv5TE, SoftVFP, `--ro_base=0x30600000`, entry symbol `AppMain`,
no Thumb-2**. Toolchain: Keil/ARM RVDS 3.1 (linker 4.1 build 894).

## 4. Syscall ABI is unchanged

Scanning `ER_RO` for unconditional `svc` instructions and comparing the 24-bit
immediates against `tools/interruptmap.txt`:

| | 2.4.2 | 15270 |
| --- | --- | --- |
| `svc` sites | 837 | 835 |
| registered sites patched | **742** | 742 |
| distinct registered numbers | **742** | 742 |
| unregistered immediates | 95 (72 numbers) | 93 (75 numbers) |
| registered numbers missing from the new image | **0** | — |
| new registered numbers | **0** | — |

So the BESTA syscall surface is byte-for-byte the same set. The 95
"unregistered" hits are literal-pool words: their immediates are values such as
`0x8A8A85`, which no sane API number produces.

## 5. It actually runs

Loaded with the unmodified runtime, `--static-syscall-patch`:

```text
loaded /workspace/fw/armfir-2.4.2.elf at 0x30600000-0x30d76000, entry 0x30604ae8
static SVC patch: 742 sites rewritten, 95 unregistered immediates, 0 failed
started pc-sdl-ipc (classes 0x7)
hardware timer initialized (period 13040000 ns)
SDKLIB: A:\WINDOW\SYSTEM\SDKLIB.DLL
check data format, read header...
launching firmware function 0x306a3984
unimplemented INDICATOR ioctl=0x101 (once per second)
```

`0x306a3984` is the 2026 equivalent of the 2025 thread entry `0x306a2cfc` —
the code moved, the handshake did not. Capturing the framebuffer produces the
same statistics as the 2025 image (225,853 non-zero bytes, 538 distinct
colours: blue title bar, white work area, six soft-key cells).

Conclusion: **PrimeShim runs HP Prime OS 2.4.2 without a single source change**,
because the ABI it depends on (entry, load address, syscall numbers, timer
period) is stable across the two releases.

## 6. Limitations of this analysis

* The 320-byte records have no explicit offset/size field; allocation is
  described by the 0xFFFFFFFF map at `+0x100`. ELF extraction sidesteps this by
  trusting the ELF header instead of the directory.
* `PRIME_MASTER.DAT` (boot stage) was not disassembled; only identified.
* `PRIME_OS.ROM` contains no ELF; its first word is `0x30000020`, suggesting a
  load address, but its structure was not reversed.
* No new syscall numbers were found, so nothing in 2.4.2 requires new shim
  work — that conclusion is limited to the registered 742.

## 7. Reproducing

```sh
unzip HP_Prime_Calculator_Firmware_20260909.zip -d fw
tools/fw_extract.py fw/PRIME_APP.DAT --list
tools/fw_extract.py fw/PRIME_APP.DAT -o fw/
make run-pc FIRMWARE=$PWD/fw/armfir.elf RUN_ARGS=--static-syscall-patch
```
