/*
 * HP Prime firmware container handling.
 *
 * Users have the update package, not an extracted ELF: they pick
 * PRIME_APP.DAT (or a copy renamed prime-app.dat) and expect the emulator to
 * figure it out.  This module turns any of the accepted inputs into a
 * validated armfir.elf.
 */
#include "firmware_image.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FW_MIN_SOURCE_BYTES (1u << 20)   /* 1 MiB: smaller than any firmware */
#define FW_SCAN_CHUNK       (1u << 20)   /* 1 MiB read window */
#define FW_MAGIC_OVERLAP    4            /* keep 3 bytes + 1 across chunks */
#define FW_APP_MAGIC        "APDSKXXX.DAT"
#define FW_MASTER_MAGIC     "EA656XXX.DAT"

typedef struct elf_window {
    uint32_t entry;
    uint64_t total_size;
} elf_window_t;

/* Validate an ELF header prefix.  `available` is how many bytes of the header
 * we actually have (52 is the full ELF32 header). */
static int parse_elf_header(const uint8_t *header, size_t available,
                            uint64_t file_size, uint64_t elf_offset,
                            elf_window_t *out) {
    uint16_t type;
    uint16_t machine;
    uint32_t shoff;
    uint16_t shentsize;
    uint16_t shnum;
    uint64_t end;

    if (available < 52) return -1;
    if (memcmp(header, "\x7f" "ELF", 4) != 0) return -1;
    if (header[4] != 1) return -1;                 /* ELFCLASS32 */
    if (header[5] != 1) return -1;                 /* little endian */

    memcpy(&type, header + 16, sizeof(type));
    memcpy(&machine, header + 18, sizeof(machine));
    if (type != 2 || machine != 40) return -1;     /* ET_EXEC, EM_ARM */

    memcpy(&shoff, header + 32, sizeof(shoff));
    memcpy(&shentsize, header + 46, sizeof(shentsize));
    memcpy(&shnum, header + 48, sizeof(shnum));
    if (shnum == 0 || shentsize < 40) return -1;

    end = elf_offset + (uint64_t)shoff + (uint64_t)shnum * shentsize;
    if (end > file_size || end < elf_offset) return -1;

    memcpy(&out->entry, header + 24, sizeof(out->entry));
    out->total_size = (uint64_t)shoff + (uint64_t)shnum * shentsize;
    return 0;
}

static ssize_t read_at(int fd, void *buffer, size_t length, uint64_t offset) {
    size_t done = 0;
    while (done < length) {
        ssize_t count = pread(fd, (uint8_t *)buffer + done, length - done,
                              (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) break;
        done += (size_t)count;
    }
    return (ssize_t)done;
}

static int write_at(int fd, const void *buffer, size_t length, uint64_t offset) {
    size_t done = 0;
    while (done < length) {
        ssize_t count = pwrite(fd, (const uint8_t *)buffer + done, length - done,
                               (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

/* Copy `length` bytes from src at `offset` to the start of dst. */
static int copy_region(int src, uint64_t offset, uint64_t length, int dst) {
    uint8_t *buffer = malloc(256 * 1024);
    uint64_t copied = 0;
    int result = 0;

    if (!buffer) return -1;
    while (copied < length) {
        size_t want = (size_t)((length - copied) > (256u * 1024u)
                                   ? (256u * 1024u)
                                   : (length - copied));
        ssize_t got = read_at(src, buffer, want, offset + copied);
        if (got <= 0) {
            result = -1;
            break;
        }
        if (write_at(dst, buffer, (size_t)got, copied) != 0) {
            result = -1;
            break;
        }
        copied += (uint64_t)got;
    }
    free(buffer);
    return result;
}

int primeshim_firmware_is_elf(const char *path) {
    uint8_t header[52];
    struct stat info;
    elf_window_t elf;
    int fd;
    ssize_t got;

    if (!path || stat(path, &info) != 0) return 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    got = read_at(fd, header, sizeof(header), 0);
    close(fd);
    if (got < 0) return 0;
    return parse_elf_header(header, (size_t)got, (uint64_t)info.st_size, 0, &elf) == 0;
}

/* Scan the file for an embedded ARM ELF.  Returns 0 and fills `offset`/`elf`. */
static int locate_embedded_elf(int fd, uint64_t file_size,
                               uint64_t *offset, elf_window_t *elf) {
    uint8_t *chunk = malloc(FW_SCAN_CHUNK + FW_MAGIC_OVERLAP);
    uint64_t position = 0;
    size_t carried = 0;
    int result = -1;

    if (!chunk) return -1;

    while (position < file_size) {
        size_t want = FW_SCAN_CHUNK;
        ssize_t got;
        size_t limit;

        if (file_size - position < want) want = (size_t)(file_size - position);
        got = read_at(fd, chunk + carried, want, position);
        if (got <= 0) break;
        limit = carried + (size_t)got;

        for (size_t i = 0; i + 4 <= limit; ++i) {
            if (memcmp(chunk + i, "\x7f" "ELF", 4) == 0) {
                uint64_t candidate = position + i - carried;
                uint8_t header[52];
                ssize_t header_bytes = read_at(fd, header, sizeof(header), candidate);
                if (header_bytes > 0 &&
                    parse_elf_header(header, (size_t)header_bytes, file_size,
                                     candidate, elf) == 0) {
                    *offset = candidate;
                    result = 0;
                    goto done;
                }
            }
        }

        position += (uint64_t)got;
        carried = limit < FW_MAGIC_OVERLAP ? limit : FW_MAGIC_OVERLAP;
        memmove(chunk, chunk + limit - carried, carried);
    }

done:
    free(chunk);
    return result;
}

static void set_message(char *message, size_t size, const char *text) {
    if (!message || size == 0) return;
    snprintf(message, size, "%s", text);
}

int primeshim_firmware_import(const char *source, const char *destination,
                              char *message, size_t message_size) {
    struct stat info;
    uint64_t offset = 0;
    uint64_t length;
    elf_window_t elf;
    char temporary[512];
    int src = -1;
    int dst = -1;
    int result;
    int fd;

    if (!source || !destination) {
        set_message(message, message_size, "missing path");
        return PRIMESHIM_FW_EARGS;
    }
    if (stat(source, &info) != 0) {
        set_message(message, message_size, "cannot stat the selected file");
        return PRIMESHIM_FW_EOPEN;
    }
    /* No size gate: third-party ports can be far smaller than the stock
     * image.  The format checks below decide whether it is usable. */
    if (info.st_size < 64) {
        set_message(message, message_size, "file is too small to be an image");
        return PRIMESHIM_FW_ETOOSMALL;
    }

    src = open(source, O_RDONLY);
    if (src < 0) {
        set_message(message, message_size, strerror(errno));
        return PRIMESHIM_FW_EOPEN;
    }

    /* Case 1: the user already picked the extracted ELF. */
    {
        uint8_t header[52];
        ssize_t got = read_at(src, header, sizeof(header), 0);
        if (got < 0) {
            close(src);
            set_message(message, message_size, "read failed");
            return PRIMESHIM_FW_EIO;
        }
        if (parse_elf_header(header, (size_t)got, (uint64_t)info.st_size, 0, &elf) == 0) {
            offset = 0;
            length = (uint64_t)info.st_size;
            PS_LOG_INFO("firmware", "imported plain ARM ELF (entry 0x%08x)",
                        elf.entry);
            goto have_elf;
        }
    }

    /* Case 2: an HP Prime container (PRIME_APP.DAT / prime-app.dat / ...). */
    {
        char magic[12];
        ssize_t got = read_at(src, magic, sizeof(magic), 0);
        if (got >= 12 && memcmp(magic, FW_MASTER_MAGIC, 12) == 0) {
            close(src);
            set_message(message, message_size,
                        "PRIME_MASTER.DAT is the boot image and contains no "
                        "firmware ELF; use PRIME_APP.DAT");
            return PRIMESHIM_FW_ENOELF;
        }
    }

    if (locate_embedded_elf(src, (uint64_t)info.st_size, &offset, &elf) != 0) {
        close(src);
        set_message(message, message_size,
                    "no 32-bit ARM ELF inside this file; expected PRIME_APP.DAT "
                    "or armfir.elf");
        return PRIMESHIM_FW_ENOELF;
    }
    length = elf.total_size;
    PS_LOG_INFO("firmware",
                "found embedded ARM ELF at 0x%llx (%llu bytes, entry 0x%08x)",
                (unsigned long long)offset, (unsigned long long)length, elf.entry);

have_elf:
    if (snprintf(temporary, sizeof(temporary), "%s.part", destination) >=
        (int)sizeof(temporary)) {
        close(src);
        set_message(message, message_size, "destination path is too long");
        return PRIMESHIM_FW_EARGS;
    }

    dst = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (dst < 0) {
        close(src);
        set_message(message, message_size, strerror(errno));
        return PRIMESHIM_FW_EOPEN;
    }

    result = copy_region(src, offset, length, dst);
    close(src);
    if (result != 0) {
        close(dst);
        unlink(temporary);
        set_message(message, message_size, "copy failed");
        return PRIMESHIM_FW_EIO;
    }
    if (fsync(dst) != 0) {
        close(dst);
        unlink(temporary);
        set_message(message, message_size, "flush failed");
        return PRIMESHIM_FW_EIO;
    }
    close(dst);

    /* Re-validate what we just wrote before publishing it. */
    fd = open(temporary, O_RDONLY);
    if (fd < 0 || !primeshim_firmware_is_elf(temporary)) {
        if (fd >= 0) close(fd);
        unlink(temporary);
        set_message(message, message_size, "extracted image failed validation");
        return PRIMESHIM_FW_EBADELF;
    }
    {
        struct stat written;
        if (fstat(fd, &written) == 0 && (uint64_t)written.st_size != length) {
            close(fd);
            unlink(temporary);
            set_message(message, message_size, "extracted image is truncated");
            return PRIMESHIM_FW_EIO;
        }
    }
    close(fd);

    if (rename(temporary, destination) != 0) {
        unlink(temporary);
        set_message(message, message_size, "cannot publish the extracted image");
        return PRIMESHIM_FW_EIO;
    }

    PS_LOG_INFO("firmware", "wrote %llu-byte firmware to %s",
                (unsigned long long)length, destination);
    set_message(message, message_size, "ok");
    return PRIMESHIM_FW_OK;
}
