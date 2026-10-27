PrimeShim assets
================

This directory intentionally contains UI resources only (keyboard skins,
fallback app icon).  No HP firmware and no A:/C: data are distributed.

On first launch the app asks the user for an official HP Prime update package
(zip) or PRIME_APP.DAT.  It extracts:

    armfir.elf                  the ARM application image
    prime_data/A/               the FAT16 A: system volume from the same DAT
    prime_data/C/               an empty user volume, initialised by the firmware

PRIME_MASTER.DAT and PRIME_OS.ROM are recognised but ignored: PrimeShim starts
at the application ELF and does not need the boot image or boot ROM.
