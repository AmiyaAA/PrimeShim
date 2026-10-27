#ifndef PRIMESHIM_FIRMWARE_IMAGE_H
#define PRIMESHIM_FIRMWARE_IMAGE_H

#include <stddef.h>

/* Accepts either a ready ARM ELF or an HP Prime update container
 * (PRIME_APP.DAT / prime-app.dat / PRIME_MASTER.DAT) and writes a validated
 * `armfir.elf` to `destination`.
 *
 * The container is a small volume: a header ("APDSKXXX.DAT"), 320-byte
 * directory records, then the file data.  The firmware itself is stored
 * verbatim, so extraction is "find the ELF magic, trust its section header
 * table for the length".  Nothing is decrypted or decompressed.
 *
 * Returns 0 on success.  On failure returns a negative code and, when message
 * is not NULL, a human readable explanation.
 */
enum {
    PRIMESHIM_FW_OK = 0,
    PRIMESHIM_FW_EOPEN = -1,      /* source or destination could not be opened */
    PRIMESHIM_FW_ETOOSMALL = -2,  /* source is not a firmware-sized file */
    PRIMESHIM_FW_ENOELF = -3,     /* no ARM ELF anywhere in the file */
    PRIMESHIM_FW_EBADELF = -4,    /* ELF found but not a 32-bit ARM executable */
    PRIMESHIM_FW_EIO = -5,        /* read/write/rename failed */
    PRIMESHIM_FW_EARGS = -6
};

int primeshim_firmware_import(const char *source, const char *destination,
                              char *message, size_t message_size);

/* True when the file already is a 32-bit ARM ELF we can load. */
int primeshim_firmware_is_elf(const char *path);

#endif
