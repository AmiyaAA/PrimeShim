#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "runtime_patcher.h"
#include "log.h"

#define ARM_SVC_MASK       0x0f000000u
#define ARM_SVC_VALUE      0x0f000000u
#define ARM_BRANCH         0x0a000000u
#define ARM_BRANCH_LINK    0x01000000u
#define ARM_SDT_MASK       0x0c000000u
#define ARM_SDT_VALUE      0x04000000u
#define DYNAMIC_PATCH_LIMIT 2048u
#define TRAMPOLINE_WORDS    32u
#ifndef SYS_USER_DISPATCH
#define SYS_USER_DISPATCH   2
#endif
#ifndef SYS_SECCOMP
#define SYS_SECCOMP         1
#endif
#ifndef PR_SYS_DISPATCH_INCLUSIVE_ON
#define PR_SYS_DISPATCH_INCLUSIVE_ON 2
#endif

typedef struct executable_range {
    uint32_t start;
    uint32_t size;
} executable_range_t;

typedef struct syscall_binding {
    uint32_t number;
    void (*handler)(void);
    unsigned flags;
    runtime_branch_kind_t branch_kind;
    const char *name;
} syscall_binding_t;

typedef struct peripheral_binding {
    uint32_t base;
    uint32_t size;
    runtime_peripheral_read_fn read;
    runtime_peripheral_write_fn write;
    void *opaque;
    unsigned flags;
    const char *name;
} peripheral_binding_t;

typedef struct peripheral_metadata {
    uint32_t instruction;
    uint32_t address;
    uint32_t binding_index;
} peripheral_metadata_t;

typedef enum patch_kind {
    PATCH_SYSCALL,
    PATCH_PERIPHERAL,
} patch_kind_t;

typedef struct pending_patch {
    patch_kind_t kind;
    uint32_t source;
    uint32_t original;
    size_t binding_index;
    peripheral_metadata_t metadata;
    uint32_t guard_base;
    uint32_t guard_offset;
    uint32_t guard_cpsr;
} pending_patch_t;

struct runtime_patcher {
    uint32_t entry;
    uint32_t image_base;
    size_t image_size;
    void *image_mapping;
    void *trampoline_mapping;
    size_t trampoline_size;

    executable_range_t *ranges;
    size_t range_count;
    size_t range_capacity;
    syscall_binding_t *syscalls;
    size_t syscall_count;
    size_t syscall_capacity;
    peripheral_binding_t *peripherals;
    size_t peripheral_count;
    size_t peripheral_capacity;
    pending_patch_t *patches;
    size_t patch_count;
    size_t patch_capacity;
    bool continue_on_segfault;

    struct sigaction previous_sigsys;
    struct sigaction previous_sigsegv;
    struct sigaction previous_sigill;
    bool signals_installed;
};

extern void runtime_peripheral_bridge(void);
static runtime_patcher_t *g_runtime_active_patcher;
volatile int primeshim_trace_calls;

/* Called from every generated trampoline when PRIMESHIM_TRACE_SYSCALLS=1.
 * r1 points at the saved r0-r3 of the guest call. */
#define TRACE_SLOTS 256
#define TRACE_REPEATS 3

void primeshim_trace_syscall(const char *name, const uint32_t *registers) {
    static const char *seen[TRACE_SLOTS];
    static unsigned counts[TRACE_SLOTS];
    unsigned i;

    if (!primeshim_trace_calls) return;

    /* Only the first few calls of each function are recorded: the steady state
     * repeats InterruptDone every 13 ms and would otherwise scroll the boot
     * sequence out of the 128 KiB ring. */
    for (i = 0; i < TRACE_SLOTS; ++i) {
        if (seen[i] == name) break;
        if (seen[i] == NULL) {
            seen[i] = name;
            break;
        }
    }
    if (i == TRACE_SLOTS) return;
    if (counts[i] >= TRACE_REPEATS) return;
    ++counts[i];

    PS_LOG_INFO("calls", "%s(r0=%08x r1=%08x r2=%08x r3=%08x)%s",
                name ? name : "?",
                registers[0], registers[1], registers[2], registers[3],
                counts[i] == TRACE_REPEATS ? " [further calls suppressed]" : "");
}

/* The kernel reads this byte on every syscall entry.  It must be per-thread:
 * firmware threads can be inside shim handlers at the same time. */
__thread volatile unsigned char runtime_syscall_selector =
    SYSCALL_DISPATCH_FILTER_ALLOW;
static __thread unsigned runtime_dispatch_mode;

static size_t page_size(void) {
    static size_t value;
    if (value == 0) {
        long result = sysconf(_SC_PAGESIZE);
        value = result > 0 ? (size_t)result : 4096u;
    }
    return value;
}

static uint32_t align_down(uint32_t value, uint32_t alignment) {
    return value & ~(alignment - 1u);
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static bool file_range_valid(size_t file_size, uint32_t offset, uint32_t size) {
    return (size_t)offset <= file_size && (size_t)size <= file_size - offset;
}

static bool image_range_valid(uint32_t image_base, size_t image_size,
                              uint32_t start, uint32_t size) {
    return start >= image_base && size <= image_size &&
           (size_t)(start - image_base) <= image_size - size;
}

/* Prints the /proc/self/maps lines overlapping [start, start+size) so a failed
 * fixed mapping says *what* is already there instead of just "File exists". */
static void log_address_conflict(uint32_t start, size_t size) {
#if defined(__linux__)
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    uint32_t end = start + (uint32_t)size;

    if (!maps) return;
    PS_LOG_ERROR("runtime", "address space around 0x%08x-0x%08x:", start, end);
    while (fgets(line, sizeof(line), maps)) {
        unsigned long first = 0;
        unsigned long last = 0;
        size_t length = strlen(line);

        if (sscanf(line, "%lx-%lx", &first, &last) != 2) continue;
        if (length && line[length - 1] == '\n') line[length - 1] = '\0';
        if (last <= (unsigned long)start || first >= (unsigned long)end) continue;
        PS_LOG_ERROR("runtime", "  %s", line);
    }
    fclose(maps);
#else
    (void)start;
    (void)size;
#endif
}

/*
 * Some 32-bit Android runtimes reserve the whole 0x12c00000-0x42c00000
 * window for the ART region space, which contains the firmware's fixed
 * 0x30600000 base.  A normal mmap then lands elsewhere and the guest cannot
 * start.  If every page in the requested range is still the untouched
 * dalvik-main-space reservation, reclaim just that slice with MAP_FIXED.
 * This is deliberately conservative: anything else (a library, stack, JIT
 * code, or a partially used heap page) keeps the normal failure path.
 */
static bool range_is_art_region_space(uint32_t start, size_t size) {
#if defined(__linux__)
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    uint32_t end = start + (uint32_t)size;
    bool saw = false;

    if (!maps) return false;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long first = 0;
        unsigned long last = 0;
        if (sscanf(line, "%lx-%lx", &first, &last) != 2) continue;
        if (last <= (unsigned long)start || first >= (unsigned long)end) continue;
        saw = true;
        if (!strstr(line, "dalvik") || !strstr(line, "region space")) {
            fclose(maps);
            return false;
        }
    }
    fclose(maps);
    if (!saw) return false;
    /* Reclaim only pages the ART heap has never touched.  A resident page
     * means the Java heap already grew into this slice; MAP_FIXED there would
     * corrupt it. */
    {
        long page = sysconf(_SC_PAGESIZE);
        uintptr_t first = (uintptr_t)start & ~((uintptr_t)page - 1u);
        uintptr_t last = ((uintptr_t)start + size + (uintptr_t)page - 1u) &
                         ~((uintptr_t)page - 1u);
        size_t pages = (size_t)((last - first) / (uintptr_t)page);
        unsigned char *resident = malloc(pages);
        bool untouched = false;
        if (resident &&
            mincore((void *)first, pages * (size_t)page, resident) == 0) {
            untouched = true;
            for (size_t i = 0; i < pages; ++i) {
                if ((resident[i] & 1u) != 0) {
                    untouched = false;
                    break;
                }
            }
        }
        free(resident);
        if (!untouched) return false;
    }
    return true;
#else
    (void)start;
    (void)size;
    return false;
#endif
}

static void *map_fixed_over_art(uint32_t address, size_t size, int fd,
                                bool file_backed) {
    void *wanted = (void *)(uintptr_t)address;
    int flags;

    if (!range_is_art_region_space(address, size)) {
        errno = EEXIST;
        return MAP_FAILED;
    }
    flags = MAP_PRIVATE | MAP_FIXED;
    if (!file_backed) flags |= MAP_ANONYMOUS;
    void *mapped = mmap(wanted, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        flags, file_backed ? fd : -1, 0);
    if (mapped == MAP_FAILED) return MAP_FAILED;
    if (mapped != wanted) {
        munmap(mapped, size);
        errno = EEXIST;
        return MAP_FAILED;
    }
    PS_LOG_WARN("runtime",
                "reclaimed %zu bytes of unused ART region space at 0x%08x",
                size, address);
    return mapped;
}

static void *map_exact_non_destructive(uint32_t address, size_t size) {
    void *wanted = (void *)(uintptr_t)address;
    void *mapped = mmap(wanted, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        return MAP_FAILED;
    }
    if (mapped != wanted) {
        munmap(mapped, size);
        mapped = map_fixed_over_art(address, size, -1, false);
        if (mapped != MAP_FAILED) return mapped;
        errno = EEXIST;
        return MAP_FAILED;
    }
    return mapped;
}

static void *map_exact_file_non_destructive(uint32_t address, size_t size,
                                            int fd) {
    void *wanted = (void *)(uintptr_t)address;
    void *mapped = mmap(wanted, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        return MAP_FAILED;
    }
    if (mapped != wanted) {
        munmap(mapped, size);
        mapped = map_fixed_over_art(address, size, fd, true);
        if (mapped != MAP_FAILED) return mapped;
        errno = EEXIST;
        return MAP_FAILED;
    }
    return mapped;
}

static int write_all_at(int fd, const uint8_t *data, size_t size,
                        off_t offset) {
    while (size != 0) {
        ssize_t written = pwrite(fd, data, size, offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        data += written;
        size -= (size_t)written;
        offset += written;
    }
    return 0;
}

static bool backing_file_is_current(const struct stat *backing,
                                    const struct stat *firmware,
                                    size_t expected_size) {
    if (backing->st_size != (off_t)expected_size) return false;
    if (backing->st_mtim.tv_sec != firmware->st_mtim.tv_sec) {
        return backing->st_mtim.tv_sec > firmware->st_mtim.tv_sec;
    }
    return backing->st_mtim.tv_nsec >= firmware->st_mtim.tv_nsec;
}

static int create_firmware_backing(const char *backing_path,
                                   const uint8_t *file,
                                   const Elf32_Phdr *phdrs,
                                   size_t phnum,
                                   uint32_t image_base,
                                   size_t image_size) {
    static const char temporary_suffix[] = ".tmp.XXXXXX";
    size_t path_length = strlen(backing_path);
    char *temporary_path = NULL;
    int output_fd = -1;
    int saved_errno = 0;
    size_t i;

    if (path_length > SIZE_MAX - sizeof(temporary_suffix)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    temporary_path = malloc(path_length + sizeof(temporary_suffix));
    if (!temporary_path) return -1;
    memcpy(temporary_path, backing_path, path_length);
    memcpy(temporary_path + path_length, temporary_suffix,
           sizeof(temporary_suffix));

    output_fd = mkstemp(temporary_path);
    if (output_fd < 0) goto out;
    if (fcntl(output_fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fchmod(output_fd, 0644) != 0 ||
        ftruncate(output_fd, (off_t)image_size) != 0) {
        goto out;
    }
    for (i = 0; i < phnum; ++i) {
        off_t output_offset;
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_filesz == 0) continue;
        output_offset = (off_t)(phdrs[i].p_vaddr - image_base);
        if (write_all_at(output_fd, file + phdrs[i].p_offset,
                         phdrs[i].p_filesz, output_offset) != 0) {
            goto out;
        }
    }
    if (fsync(output_fd) != 0 || rename(temporary_path, backing_path) != 0) {
        goto out;
    }
    (void)posix_fadvise(output_fd, 0, 0, POSIX_FADV_DONTNEED);
    close(output_fd);
    free(temporary_path);
    return 0;

out:
    saved_errno = errno;
    if (output_fd >= 0) close(output_fd);
    if (temporary_path) unlink(temporary_path);
    free(temporary_path);
    errno = saved_errno;
    return -1;
}

static int grow_array(void **array, size_t *capacity,
                      size_t needed, size_t element_size) {
    size_t new_capacity;
    void *new_array;

    if (needed <= *capacity) {
        return 0;
    }
    new_capacity = *capacity ? *capacity * 2u : 16u;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2u) {
            errno = ENOMEM;
            return -1;
        }
        new_capacity *= 2u;
    }
    new_array = realloc(*array, new_capacity * element_size);
    if (!new_array) {
        return -1;
    }
    *array = new_array;
    *capacity = new_capacity;
    return 0;
}

static int add_range(runtime_patcher_t *patcher, uint32_t start, uint32_t size) {
    if (size == 0) {
        return 0;
    }
    if (grow_array((void **)&patcher->ranges, &patcher->range_capacity,
                   patcher->range_count + 1u, sizeof(*patcher->ranges)) != 0) {
        return -1;
    }
    patcher->ranges[patcher->range_count].start = start;
    patcher->ranges[patcher->range_count].size = size;
    ++patcher->range_count;
    return 0;
}

runtime_patcher_t *runtime_patcher_create(void) {
    return calloc(1, sizeof(runtime_patcher_t));
}

void runtime_patcher_set_continue_on_segfault(runtime_patcher_t *patcher,
                                              bool enabled) {
    if (patcher) {
        patcher->continue_on_segfault = enabled;
    }
}

void runtime_patcher_destroy(runtime_patcher_t *patcher) {
    if (!patcher) {
        return;
    }
    runtime_patcher_leave_firmware_thread();
    if (patcher->signals_installed) {
        sigaction(SIGSYS, &patcher->previous_sigsys, NULL);
        sigaction(SIGSEGV, &patcher->previous_sigsegv, NULL);
        sigaction(SIGILL, &patcher->previous_sigill, NULL);
    }
    if (g_runtime_active_patcher == patcher) {
        g_runtime_active_patcher = NULL;
    }
    if (patcher->trampoline_mapping) {
        munmap(patcher->trampoline_mapping, patcher->trampoline_size);
    }
    if (patcher->image_mapping) {
        munmap(patcher->image_mapping, patcher->image_size);
    }
    free(patcher->ranges);
    free(patcher->syscalls);
    free(patcher->peripherals);
    free(patcher->patches);
    free(patcher);
}

int runtime_patcher_load_elf(runtime_patcher_t *patcher, const char *path) {
    int fd = -1;
    int backing_fd = -1;
    struct stat st;
    struct stat backing_st;
    uint8_t *file = MAP_FAILED;
    char *backing_path = NULL;
    const Elf32_Ehdr *ehdr;
    const Elf32_Phdr *phdrs;
    uint32_t min_address = UINT32_MAX;
    uint32_t max_address = 0;
    uint32_t page = (uint32_t)page_size();
    size_t i;
    int result = -1;
    bool file_backed = false;
    bool backing_current = false;

    if (!patcher || !path || patcher->image_mapping) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0) {
        PS_LOG_ERRNO_ERROR("runtime", errno, "cannot open firmware %s", path);
        goto out;
    }
    if (st.st_size < (off_t)sizeof(Elf32_Ehdr)) {
        PS_LOG_ERROR("runtime", "%s is too small to be an ELF image", path);
        goto out;
    }
    file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) {
        PS_LOG_ERRNO_ERROR("runtime", errno, "cannot map firmware ELF");
        goto out;
    }
    ehdr = (const Elf32_Ehdr *)file;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_machine != EM_ARM ||
        ehdr->e_phentsize != sizeof(Elf32_Phdr) ||
        !file_range_valid((size_t)st.st_size, ehdr->e_phoff,
                          (uint32_t)ehdr->e_phnum * sizeof(Elf32_Phdr))) {
        PS_LOG_ERROR("runtime", "%s is not a valid ARM ELF32 image", path);
        goto out;
    }
    phdrs = (const Elf32_Phdr *)(file + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; ++i) {
        uint32_t segment_end;
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }
        if (phdrs[i].p_filesz > phdrs[i].p_memsz ||
            !file_range_valid((size_t)st.st_size, phdrs[i].p_offset,
                              phdrs[i].p_filesz) ||
            phdrs[i].p_vaddr > UINT32_MAX - phdrs[i].p_memsz) {
            PS_LOG_ERROR("runtime", "%s has a malformed PT_LOAD segment",
                         path);
            goto out;
        }
        segment_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (segment_end > UINT32_MAX - (page - 1u)) {
            PS_LOG_ERROR("runtime",
                         "%s has a PT_LOAD that cannot be page-aligned", path);
            goto out;
        }
        if (align_down(phdrs[i].p_vaddr, page) < min_address) {
            min_address = align_down(phdrs[i].p_vaddr, page);
        }
        if (align_up(segment_end, page) > max_address) {
            max_address = align_up(segment_end, page);
        }
    }
    if (min_address == UINT32_MAX || max_address <= min_address) {
        PS_LOG_ERROR("runtime", "%s has no loadable ELF image", path);
        goto out;
    }
    patcher->image_size = (size_t)(max_address - min_address);

    if (strlen(path) <= SIZE_MAX - sizeof(".map")) {
        size_t backing_path_size = strlen(path) + sizeof(".map");
        backing_path = malloc(backing_path_size);
        if (backing_path) {
            snprintf(backing_path, backing_path_size, "%s.map", path);
            backing_fd = open(backing_path, O_RDONLY);
            backing_current = backing_fd >= 0 &&
                              fstat(backing_fd, &backing_st) == 0 &&
                              backing_file_is_current(&backing_st, &st,
                                                      patcher->image_size);
            if (!backing_current) {
                int create_error;
                if (backing_fd >= 0) {
                    close(backing_fd);
                    backing_fd = -1;
                }
                if (create_firmware_backing(backing_path, file, phdrs,
                                            ehdr->e_phnum, min_address,
                                            patcher->image_size) == 0) {
                    PS_LOG_INFO("runtime", "generated firmware map %s",
                                backing_path);
                    backing_fd = open(backing_path, O_RDONLY);
                    backing_current = backing_fd >= 0 &&
                                      fstat(backing_fd, &backing_st) == 0 &&
                                      backing_st.st_size ==
                                          (off_t)patcher->image_size;
                } else {
                    create_error = errno;
                    PS_LOG_ERRNO_WARN(
                        "runtime", create_error,
                        "cannot generate firmware map %s; using anonymous RAM",
                        backing_path);
                }
            }
            if (backing_current) {
                patcher->image_mapping = map_exact_file_non_destructive(
                    min_address, patcher->image_size, backing_fd);
                file_backed = patcher->image_mapping != MAP_FAILED;
            }
        }
    }

    if (!file_backed) {
        patcher->image_mapping = map_exact_non_destructive(min_address,
                                                           patcher->image_size);
    }
    if (patcher->image_mapping == MAP_FAILED &&
        primeshim_release_address_window != NULL &&
        primeshim_release_address_window(min_address, patcher->image_size) == 0) {
        /* The platform was holding this window for us; try again now. */
        PS_LOG_INFO("runtime", "released the platform reservation at 0x%08x",
                    min_address);
        patcher->image_mapping = file_backed
            ? map_exact_file_non_destructive(min_address, patcher->image_size,
                                             backing_fd)
            : map_exact_non_destructive(min_address, patcher->image_size);
    }
    if (patcher->image_mapping == MAP_FAILED) {
        patcher->image_mapping = NULL;
        PS_LOG_ERRNO_ERROR("runtime", errno,
                           "cannot map %s at 0x%08x", path, min_address);
        log_address_conflict(min_address, patcher->image_size);
        goto out;
    }
    patcher->image_base = min_address;
    patcher->entry = ehdr->e_entry;

    if (!file_backed) {
        for (i = 0; i < ehdr->e_phnum; ++i) {
            if (phdrs[i].p_type != PT_LOAD) {
                continue;
            }
            memcpy((void *)(uintptr_t)phdrs[i].p_vaddr,
                   file + phdrs[i].p_offset, phdrs[i].p_filesz);
        }
    }
    if (ehdr->e_shentsize == sizeof(Elf32_Shdr) && ehdr->e_shnum != 0 &&
        file_range_valid((size_t)st.st_size, ehdr->e_shoff,
                         (uint32_t)ehdr->e_shnum * sizeof(Elf32_Shdr))) {
        const Elf32_Shdr *shdrs = (const Elf32_Shdr *)(file + ehdr->e_shoff);
        for (i = 0; i < ehdr->e_shnum; ++i) {
            if ((shdrs[i].sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) ==
                    (SHF_ALLOC | SHF_EXECINSTR) &&
                image_range_valid(patcher->image_base, patcher->image_size,
                                  shdrs[i].sh_addr, shdrs[i].sh_size) &&
                add_range(patcher, shdrs[i].sh_addr, shdrs[i].sh_size) != 0) {
                goto out;
            }
        }
    }
    if (patcher->range_count == 0 ||
        !image_range_valid(patcher->image_base, patcher->image_size,
                           patcher->entry & ~1u, 1u)) {
        PS_LOG_ERROR("runtime",
                     "%s has no valid executable range or entry point", path);
        goto out;
    }
    if (patcher->range_count == 0) {
        for (i = 0; i < ehdr->e_phnum; ++i) {
            if (phdrs[i].p_type == PT_LOAD && (phdrs[i].p_flags & PF_X) != 0 &&
                add_range(patcher, phdrs[i].p_vaddr, phdrs[i].p_filesz) != 0) {
                goto out;
            }
        }
    }
    PS_LOG_INFO("runtime",
                "loaded %s at 0x%08x-0x%08x, entry 0x%08x (%s-backed)",
                path, min_address, max_address, patcher->entry,
                file_backed ? "file" : "anonymous");
    result = 0;

out:
    if (file != MAP_FAILED) {
        munmap(file, (size_t)st.st_size);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (backing_fd >= 0) {
        close(backing_fd);
    }
    free(backing_path);
    return result;
}

int runtime_patcher_register_syscall(runtime_patcher_t *patcher,
                                     uint32_t number,
                                     void (*handler)(void),
                                     unsigned flags,
                                     runtime_branch_kind_t branch_kind,
                                     const char *name) {
    size_t i;
    syscall_binding_t *binding;

    if (!patcher || !handler || number > 0x00ffffffu ||
        (branch_kind != RUNTIME_BRANCH && branch_kind != RUNTIME_BRANCH_LINK)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < patcher->syscall_count; ++i) {
        if (patcher->syscalls[i].number == number) {
            syscall_binding_t *existing = &patcher->syscalls[i];
            existing->handler = handler;
            existing->flags = flags;
            existing->branch_kind = branch_kind;
            existing->name = name;
            return 0;
        }
    }
    if (grow_array((void **)&patcher->syscalls, &patcher->syscall_capacity,
                   patcher->syscall_count + 1u,
                   sizeof(*patcher->syscalls)) != 0) {
        return -1;
    }
    binding = &patcher->syscalls[patcher->syscall_count++];
    binding->number = number;
    binding->handler = handler;
    binding->flags = flags;
    binding->branch_kind = branch_kind;
    binding->name = name;
    return 0;
}

int runtime_patcher_register_peripheral(runtime_patcher_t *patcher,
                                        uint32_t base,
                                        uint32_t size,
                                        runtime_peripheral_read_fn read,
                                        runtime_peripheral_write_fn write,
                                        void *opaque,
                                        unsigned flags,
                                        const char *name) {
    peripheral_binding_t *binding;

    if (!patcher || size == 0 || base > UINT32_MAX - size ||
        (!read && !write)) {
        errno = EINVAL;
        return -1;
    }
    if (grow_array((void **)&patcher->peripherals,
                   &patcher->peripheral_capacity,
                   patcher->peripheral_count + 1u,
                   sizeof(*patcher->peripherals)) != 0) {
        return -1;
    }
    binding = &patcher->peripherals[patcher->peripheral_count++];
    binding->base = base;
    binding->size = size;
    binding->read = read;
    binding->write = write;
    binding->opaque = opaque;
    binding->flags = flags;
    binding->name = name;
    return 0;
}

static ssize_t find_syscall(const runtime_patcher_t *patcher, uint32_t number) {
    size_t i;
    for (i = 0; i < patcher->syscall_count; ++i) {
        if (patcher->syscalls[i].number == number) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static ssize_t find_peripheral(const runtime_patcher_t *patcher,
                               uint32_t address, unsigned width) {
    size_t i;
    for (i = 0; i < patcher->peripheral_count; ++i) {
        const peripheral_binding_t *binding = &patcher->peripherals[i];
        if (address >= binding->base && width <= binding->size &&
            address - binding->base <= binding->size - width) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static uint32_t rotate_right(uint32_t value, unsigned shift) {
    shift &= 31u;
    return shift ? (value >> shift) | (value << (32u - shift)) : value;
}

static int encode_branch(uint32_t source, uint32_t target, uint32_t condition,
                         bool link, uint32_t *encoded) {
    int64_t delta = (int64_t)target - ((int64_t)source + 8);
    if ((delta & 3) != 0 || delta < -33554432LL || delta > 33554428LL) {
        errno = ERANGE;
        return -1;
    }
    *encoded = condition | ARM_BRANCH |
               (link ? ARM_BRANCH_LINK : 0u) |
               ((uint32_t)(delta >> 2) & 0x00ffffffu);
    return 0;
}

static int emit_syscall_trampoline(runtime_patcher_t *patcher,
                                   const pending_patch_t *patch,
                                   uint32_t *trampoline) {
    const syscall_binding_t *binding = &patcher->syscalls[patch->binding_index];
    uint32_t trampoline_address = (uint32_t)(uintptr_t)trampoline;
    uint32_t branch;
    size_t base = 0;

    if ((binding->flags & RUNTIME_SYSCALL_BESTA_FRAME) != 0) {
        trampoline[0] = 0xe28dd008u; /* add sp, sp, #8 */
        base = 1;
    }
    if (base + 12 <= TRAMPOLINE_WORDS) {
        /* Keep SP 8-byte aligned for the varargs log call:
         *   push {r0-r3, lr} / sub sp, #4  -> 24 bytes
         * r1 points at the saved r0-r3, so the C helper can print the args. */
        trampoline[base + 0] = 0xe92d400fu;  /* push {r0-r3, lr}        */
        trampoline[base + 1] = 0xe24dd004u;  /* sub  sp, sp, #4         */
        trampoline[base + 2] = 0xe28d1004u;  /* add  r1, sp, #4         */
        trampoline[base + 3] = 0xe59f0014u;  /* ldr  r0, [pc, #20] name */
        trampoline[base + 4] = 0xe59fc014u;  /* ldr  r12,[pc, #20] fn   */
        trampoline[base + 5] = 0xe12fff3cu;  /* blx  r12                */
        trampoline[base + 6] = 0xe28dd004u;  /* add  sp, sp, #4         */
        trampoline[base + 7] = 0xe8bd400fu;  /* pop  {r0-r3, lr}        */
        trampoline[base + 10] = (uint32_t)(uintptr_t)(binding->name ? binding->name : "?");
        trampoline[base + 11] = (uint32_t)(uintptr_t)&primeshim_trace_syscall;
        base += 8;
    }
    trampoline[base + 0] = 0xe51ff004u; /* ldr pc, [pc, #-4] */
    trampoline[base + 1] = (uint32_t)(uintptr_t)binding->handler;
    if (encode_branch(patch->source, trampoline_address,
                      patch->original & 0xf0000000u,
                      binding->branch_kind == RUNTIME_BRANCH_LINK,
                      &branch) != 0) {
        PS_LOG_ERROR("runtime", "trampoline for %s is outside ARM B range",
                     binding->name ? binding->name : "syscall");
        return -1;
    }
    __builtin___clear_cache((char *)trampoline,
                            (char *)trampoline + TRAMPOLINE_WORDS * 4u);
    *(volatile uint32_t *)(uintptr_t)patch->source = branch;
    __builtin___clear_cache((char *)(uintptr_t)patch->source,
                            (char *)(uintptr_t)patch->source + 4u);
    return 0;
}

typedef struct literal_fixup {
    size_t instruction_index;
    unsigned destination;
    uint32_t value;
} literal_fixup_t;

static int emit_peripheral_trampoline(const pending_patch_t *patch,
                                      uint32_t *trampoline) {
    uint32_t trampoline_address = (uint32_t)(uintptr_t)trampoline;
    uint32_t instruction = patch->metadata.instruction;
    unsigned rn = (instruction >> 16) & 15u;
    unsigned rm = instruction & 15u;
    bool register_offset = (instruction & (1u << 25)) != 0;
    unsigned saved_flags;
    unsigned literal_reg;
    uint32_t saved_mask;
    literal_fixup_t literals[8];
    size_t mismatch_branches[3];
    size_t literal_count = 0;
    size_t mismatch_count = 0;
    size_t count = 0;
    size_t unmatched;
    size_t i;
    uint32_t branch;

    /* Pick two registers that do not participate in address calculation.
     * They are saved before the generated guard and restored on both paths. */
    for (saved_flags = 0; saved_flags <= 12u; ++saved_flags) {
        if (saved_flags != rn && (!register_offset || saved_flags != rm)) break;
    }
    for (literal_reg = saved_flags + 1u; literal_reg <= 12u; ++literal_reg) {
        if (literal_reg != rn && (!register_offset || literal_reg != rm)) break;
    }
    if (saved_flags > 12u || literal_reg > 12u) {
        errno = ENOTSUP;
        return -1;
    }
    saved_mask = (1u << saved_flags) | (1u << literal_reg);

#define EMIT(word) (trampoline[count++] = (word))
#define EMIT_LITERAL(reg, constant) do { \
        literals[literal_count].instruction_index = count; \
        literals[literal_count].destination = (reg); \
        literals[literal_count].value = (constant); \
        literal_count++; \
        EMIT(0); \
    } while (0)
#define EMIT_MISMATCH(condition) do { \
        mismatch_branches[mismatch_count++] = count; \
        EMIT((condition) | ARM_BRANCH); \
    } while (0)

    EMIT(0xe92d0000u | saved_mask);                 /* push scratch pair */
    EMIT(0xe10f0000u | (saved_flags << 12));        /* mrs flags, cpsr */
    EMIT_LITERAL(literal_reg, patch->guard_base);
    EMIT(0xe1500000u | (rn << 16) | literal_reg);   /* cmp rn, expected */
    EMIT_MISMATCH(0x10000000u);                     /* bne unmatched */

    if (register_offset && rm != rn) {
        EMIT_LITERAL(literal_reg, patch->guard_offset);
        EMIT(0xe1500000u | (rm << 16) | literal_reg);
        EMIT_MISMATCH(0x10000000u);
    }
    if (register_offset && ((instruction >> 5) & 3u) == 3u &&
        ((instruction >> 7) & 31u) == 0u) {
        EMIT_LITERAL(literal_reg, 1u << 29);
        EMIT(0xe1100000u | (saved_flags << 16) | literal_reg); /* tst C */
        EMIT_MISMATCH((patch->guard_cpsr & (1u << 29)) ?
                      0x00000000u : 0x10000000u);
    }

    EMIT(0xe128f000u | saved_flags);                /* msr cpsr_f, flags */
    EMIT(0xe8bd0000u | saved_mask);                 /* pop scratch pair */
    EMIT(0xe92d5000u);                              /* push {r12, lr} */
    EMIT_LITERAL(12u, (uint32_t)(uintptr_t)&patch->metadata);
    EMIT(0xe28fe000u);                              /* adr lr, bridge_return */
    EMIT_LITERAL(15u, (uint32_t)(uintptr_t)runtime_peripheral_bridge);
    EMIT(0xe8bd5000u);                              /* bridge_return: pop */
    EMIT_LITERAL(15u, patch->source + 4u);

    unmatched = count;
    EMIT(0xe128f000u | saved_flags);
    EMIT(0xe8bd0000u | saved_mask);
    EMIT(patch->original);                          /* real unregistered access */
    EMIT_LITERAL(15u, patch->source + 4u);

    if (count + literal_count > TRAMPOLINE_WORDS) {
        errno = E2BIG;
        return -1;
    }
    for (i = 0; i < literal_count; ++i) {
        uint32_t instruction_address = trampoline_address +
                                       (uint32_t)literals[i].instruction_index * 4u;
        uint32_t literal_address = trampoline_address + (uint32_t)count * 4u;
        uint32_t delta = literal_address - (instruction_address + 8u);

        if (delta > 0xfffu) {
            errno = ERANGE;
            return -1;
        }
        trampoline[literals[i].instruction_index] =
            0xe59f0000u | (literals[i].destination << 12) | delta;
        trampoline[count++] = literals[i].value;
    }
    for (i = 0; i < mismatch_count; ++i) {
        uint32_t condition = trampoline[mismatch_branches[i]] & 0xf0000000u;
        if (encode_branch(trampoline_address +
                          (uint32_t)mismatch_branches[i] * 4u,
                          trampoline_address + (uint32_t)unmatched * 4u,
                          condition, false,
                          &trampoline[mismatch_branches[i]]) != 0) {
            return -1;
        }
    }
#undef EMIT_MISMATCH
#undef EMIT_LITERAL
#undef EMIT

    if (encode_branch(patch->source, trampoline_address,
                      patch->original & 0xf0000000u, false, &branch) != 0) {
        PS_LOG_ERROR("runtime",
                     "peripheral trampoline at 0x%08x is outside ARM B range",
                     trampoline_address);
        return -1;
    }
    __builtin___clear_cache((char *)trampoline,
                            (char *)trampoline + TRAMPOLINE_WORDS * 4u);
    *(volatile uint32_t *)(uintptr_t)patch->source = branch;
    __builtin___clear_cache((char *)(uintptr_t)patch->source,
                            (char *)(uintptr_t)patch->source + 4u);
    return 0;
}

static uint32_t context_register(const ucontext_t *context, unsigned reg) {
    const mcontext_t *machine = &context->uc_mcontext;

    switch (reg) {
    case 0: return machine->arm_r0;
    case 1: return machine->arm_r1;
    case 2: return machine->arm_r2;
    case 3: return machine->arm_r3;
    case 4: return machine->arm_r4;
    case 5: return machine->arm_r5;
    case 6: return machine->arm_r6;
    case 7: return machine->arm_r7;
    case 8: return machine->arm_r8;
    case 9: return machine->arm_r9;
    case 10: return machine->arm_r10;
    case 11: return machine->arm_fp;
    case 12: return machine->arm_ip;
    case 13: return machine->arm_sp;
    case 14: return machine->arm_lr;
    default: return machine->arm_pc + 8u;
    }
}

static bool decode_context_sdt(uint32_t instruction, const ucontext_t *context,
                               uint32_t *address, unsigned *width,
                               bool *load) {
    unsigned rn = (instruction >> 16) & 15u;
    unsigned rd = (instruction >> 12) & 15u;
    uint32_t base;
    uint32_t offset;
    uint32_t adjusted;

    if ((instruction >> 28) == 15u ||
        (instruction & ARM_SDT_MASK) != ARM_SDT_VALUE ||
        rn >= 13u || rd >= 13u) {
        return false;
    }
    base = context_register(context, rn);
    if ((instruction & (1u << 25)) == 0) {
        offset = instruction & 0xfffu;
    } else {
        unsigned rm = instruction & 15u;
        unsigned shift = (instruction >> 7) & 31u;
        unsigned shift_type = (instruction >> 5) & 3u;
        uint32_t value;

        if ((instruction & (1u << 4)) != 0 || rm >= 13u) {
            return false;
        }
        value = context_register(context, rm);
        switch (shift_type) {
        case 0:
            offset = shift ? value << shift : value;
            break;
        case 1:
            offset = shift ? value >> shift : 0;
            break;
        case 2:
            offset = shift ? (uint32_t)((int32_t)value >> shift) :
                     ((value & 0x80000000u) ? UINT32_MAX : 0);
            break;
        default:
            offset = shift ? rotate_right(value, shift) :
                     (((context->uc_mcontext.arm_cpsr & (1u << 29)) << 2) |
                      (value >> 1));
            break;
        }
    }
    adjusted = (instruction & (1u << 23)) ?
               base + offset : base - offset;
    *address = (instruction & (1u << 24)) ? adjusted : base;
    *width = (instruction & (1u << 22)) ? 1u : 4u;
    *load = (instruction & (1u << 20)) != 0;
    return true;
}

static pending_patch_t *reserve_patch(runtime_patcher_t *patcher,
                                      uint32_t **trampoline) {
    size_t index = __sync_fetch_and_add(&patcher->patch_count, 1u);

    if (index >= patcher->patch_capacity) {
        return NULL;
    }
    *trampoline = (uint32_t *)((uint8_t *)patcher->trampoline_mapping +
                  index * TRAMPOLINE_WORDS * sizeof(uint32_t));
    return &patcher->patches[index];
}

static bool patch_caught_syscall(runtime_patcher_t *patcher, uint32_t source,
                                 ucontext_t *context) {
    uint32_t instruction;
    uint32_t *trampoline;
    ssize_t binding_index;
    pending_patch_t *patch;

    if (!source || (source & 3u) != 0 ||
        (context->uc_mcontext.arm_cpsr & (1u << 5)) != 0) {
        return false;
    }
    instruction = *(const volatile uint32_t *)(uintptr_t)source;
    if ((instruction >> 28) == 15u ||
        (instruction & ARM_SVC_MASK) != ARM_SVC_VALUE) {
        return false;
    }
    binding_index = find_syscall(patcher, instruction & 0x00ffffffu);
    if (binding_index < 0) {
        return false;
    }
    patch = reserve_patch(patcher, &trampoline);
    if (!patch) {
        return false;
    }
    memset(patch, 0, sizeof(*patch));
    patch->kind = PATCH_SYSCALL;
    patch->source = source;
    patch->original = instruction;
    patch->binding_index = (size_t)binding_index;
    if (emit_syscall_trampoline(patcher, patch, trampoline) != 0) {
        return false;
    }

    /* SUD reports the architectural PC after the SVC.  Retry at the newly
     * installed branch so the first invocation follows the same path too. */
    context->uc_mcontext.arm_pc = source;
    return true;
}

static void set_context_register(ucontext_t *context, unsigned reg,
                                   uint32_t value) {
    mcontext_t *machine = &context->uc_mcontext;

    switch (reg) {
    case 0: machine->arm_r0 = value; break;
    case 1: machine->arm_r1 = value; break;
    case 2: machine->arm_r2 = value; break;
    case 3: machine->arm_r3 = value; break;
    case 4: machine->arm_r4 = value; break;
    case 5: machine->arm_r5 = value; break;
    case 6: machine->arm_r6 = value; break;
    case 7: machine->arm_r7 = value; break;
    case 8: machine->arm_r8 = value; break;
    case 9: machine->arm_r9 = value; break;
    case 10: machine->arm_r10 = value; break;
    case 11: machine->arm_fp = value; break;
    case 12: machine->arm_ip = value; break;
    case 13: machine->arm_sp = value; break;
    case 14: machine->arm_lr = value; break;
    default: break;
    }
}

/*
 * Android refuses mmap at address zero (mmap_min_addr), so the four-byte
 * sentinel is emulated as a peripheral.  The fault path originally only knew
 * single LDR/STR transfers.  Older firmware (0915) reaches the sentinel
 * through a memcpy built from LDM/STM, which faults on the whole block.  Emulate
 * the block transfer directly when every word lands in the zero-page binding.
 */
static bool patch_faulting_block_transfer(runtime_patcher_t *patcher,
                                          siginfo_t *information,
                                          ucontext_t *context) {
    uint32_t source = context->uc_mcontext.arm_pc;
    uint32_t instruction;
    uint32_t list;
    uint32_t base;
    uint32_t first;
    uint32_t last;
    uint32_t address;
    unsigned rn;
    unsigned count;
    unsigned reg;
    bool pre;
    bool up;
    bool writeback;
    bool load;
    ssize_t binding_index;
    const peripheral_binding_t *binding;

    if ((source & 3u) != 0 ||
        (context->uc_mcontext.arm_cpsr & (1u << 5)) != 0) {
        return false;
    }
    instruction = *(const volatile uint32_t *)(uintptr_t)source;
    if ((instruction >> 28) == 15u ||
        (instruction & 0x0e000000u) != 0x08000000u) {
        return false;
    }
    if ((instruction & (1u << 22)) != 0) {      /* user-bank transfer */
        return false;
    }
    rn = (instruction >> 16) & 15u;
    list = instruction & 0xffffu;
    if (rn >= 13u || list == 0 || (list & (1u << 15)) != 0) {
        return false;                            /* SP base or PC in list */
    }
    count = (unsigned)__builtin_popcount(list);
    pre = (instruction & (1u << 24)) != 0;
    up = (instruction & (1u << 23)) != 0;
    writeback = (instruction & (1u << 21)) != 0;
    load = (instruction & (1u << 20)) != 0;
    base = context_register(context, rn);

    if (up) {
        first = base + (pre ? 4u : 0u);
    } else {
        first = base - (pre ? 4u * count : 4u * (count - 1u));
    }
    last = first + 4u * (count - 1u);

    binding_index = find_peripheral(patcher, first, 4);
    if (binding_index < 0) return false;
    binding = &patcher->peripherals[binding_index];
    if (first < binding->base || last < first ||
        last + 4u > binding->base + binding->size) {
        return false;
    }
    if (load ? !binding->read : !binding->write) return false;
    if ((uint32_t)(uintptr_t)information->si_addr < first ||
        (uint32_t)(uintptr_t)information->si_addr > last) {
        return false;
    }

    address = first;
    for (reg = 0; reg < 15u; ++reg) {
        if ((list & (1u << reg)) == 0) continue;
        if (load) {
            set_context_register(context, reg,
                                 binding->read(binding->opaque, address, 4));
        } else {
            binding->write(binding->opaque, address, 4,
                           context_register(context, reg));
        }
        address += 4u;
    }
    if (writeback && !(load && (list & (1u << rn)) != 0)) {
        set_context_register(context, rn,
                             base + (up ? 4u * count : (uint32_t)(-(int32_t)(4u * count))));
    }
    context->uc_mcontext.arm_pc = source + 4u;
    return true;
}

/*
 * Android/W^X leaves the guest heap non-executable.  The 0915 debug/backdoor
 * interface uploads an ARM shellcode loader with lmalloc() and branches to it;
 * that is a normal function call on the calculator but an instruction-fetch
 * fault on Android (fault address == PC, inside a writable anonymous mapping).
 * Add PROT_EXEC to that one page and retry.  This is intentionally limited to
 * writable, non-executable anonymous pages; stack, libraries and the firmware
 * image are not made executable by this path.
 */
static bool mapping_is_writable_anonymous(uint32_t address, bool *found) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    bool writable = false;
    bool executable = false;
    bool blocked = false;

    *found = false;
    if (!maps) return false;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long first = 0;
        unsigned long last = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &first, &last, perms) != 3) continue;
        if ((unsigned long)address < first || (unsigned long)address >= last) {
            continue;
        }
        *found = true;
        writable = perms[1] == 'w';
        executable = perms[2] == 'x';
        if (strstr(line, "[stack]") || strstr(line, "dalvik") ||
            strstr(line, "/dev/") || strstr(line, ".so")) {
            blocked = true;
        }
        break;
    }
    fclose(maps);
    return *found && writable && !executable && !blocked;
}

static bool make_heap_page_executable(siginfo_t *information,
                                      ucontext_t *context) {
    uintptr_t fault = (uintptr_t)information->si_addr;
    uintptr_t pc = (uintptr_t)context->uc_mcontext.arm_pc;
    long page;
    uintptr_t start;
    bool found = false;

    if (fault == 0 || (fault & ~1u) != (pc & ~1u)) return false;
    if (!mapping_is_writable_anonymous((uint32_t)fault, &found)) return false;
    page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return false;
    start = fault & ~((uintptr_t)page - 1u);
    (void)start;
    /* Do NOT mprotect this page executable.  Heap code is not covered by the
     * firmware SVC patch (and is far outside the trampoline branch range), so
     * its BESTA SVCs would become raw Linux syscalls.  That lets arbitrary
     * native code corrupt the app/ART and has hung/rebooted devices.  Keep the
     * guest crash contained instead; a sandboxed guest process is required to
     * run heap-loaded code safely. */
    PS_LOG_WARN("runtime",
                "blocked execution of guest heap page 0x%08lx at pc=0x%08lx "
                "(unsafe in-process; use an isolated guest)",
                (unsigned long)start, (unsigned long)pc);
    return false;
}

static bool patch_faulting_peripheral(runtime_patcher_t *patcher,
                                      siginfo_t *information,
                                      ucontext_t *context) {
    uint32_t source = context->uc_mcontext.arm_pc;
    uint32_t instruction;
    uint32_t address;
    uint32_t *trampoline;
    unsigned width;
    bool load;
    ssize_t binding_index;
    pending_patch_t *patch;
    const peripheral_binding_t *binding;

    if ((source & 3u) != 0 ||
        (context->uc_mcontext.arm_cpsr & (1u << 5)) != 0) {
        return false;
    }
    instruction = *(const volatile uint32_t *)(uintptr_t)source;
    if (!decode_context_sdt(instruction, context, &address, &width, &load) ||
        (uint32_t)(uintptr_t)information->si_addr != address) {
        return false;
    }
    binding_index = find_peripheral(patcher, address, width);
    if (binding_index < 0) {
        return false;
    }
    binding = &patcher->peripherals[binding_index];
    if ((load && !binding->read) || (!load && !binding->write)) {
        return false;
    }
    patch = reserve_patch(patcher, &trampoline);
    if (!patch) {
        return false;
    }
    memset(patch, 0, sizeof(*patch));
    patch->kind = PATCH_PERIPHERAL;
    patch->source = source;
    patch->original = instruction;
    patch->binding_index = (size_t)binding_index;
    patch->metadata.instruction = instruction;
    patch->metadata.address = address;
    patch->metadata.binding_index = (uint32_t)binding_index;
    patch->guard_base = context_register(context, (instruction >> 16) & 15u);
    if ((instruction & (1u << 25)) != 0) {
        patch->guard_offset = context_register(context, instruction & 15u);
    }
    patch->guard_cpsr = context->uc_mcontext.arm_cpsr;
    return emit_peripheral_trampoline(patch, trampoline) == 0;
}

static char *signal_text(char *out, const char *text) {
    while (*text) {
        *out++ = *text++;
    }
    return out;
}

static char *signal_hex(char *out, uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    int shift;

    *out++ = '0';
    *out++ = 'x';
    for (shift = 28; shift >= 0; shift -= 4) {
        *out++ = digits[(value >> shift) & 15u];
    }
    return out;
}

static void report_signal(const char *action, int signal_number,
                          const siginfo_t *information,
                          const ucontext_t *context) {
    const char *name = signal_number == SIGSEGV ? "SIGSEGV" :
                       (signal_number == SIGILL ? "SIGILL" : "SIGSYS");
    char message[160];
    char *out = signal_text(message, "[runtime] ");

    out = signal_text(out, action);
    out = signal_text(out, name);
    if (information) {
        out = signal_text(out, ": address=");
        out = signal_hex(out, (uint32_t)(uintptr_t)information->si_addr);
        out = signal_text(out, " code=");
        out = signal_hex(out, (uint32_t)information->si_code);
    }
    if (context) {
        out = signal_text(out, " pc=");
        out = signal_hex(out, context->uc_mcontext.arm_pc);
        out = signal_text(out, " lr=");
        out = signal_hex(out, context->uc_mcontext.arm_lr);
        out = signal_text(out, " sp=");
        out = signal_hex(out, context->uc_mcontext.arm_sp);
    }
    *out++ = '\n';
    (void)write(STDERR_FILENO, message, (size_t)(out - message));
}

static bool is_runtime_arm_pc(const runtime_patcher_t *patcher,
                              const ucontext_t *context) {
    uint32_t pc = context->uc_mcontext.arm_pc;
    uint32_t trampoline = (uint32_t)(uintptr_t)patcher->trampoline_mapping;

    if ((pc & 3u) != 0 || (context->uc_mcontext.arm_cpsr & (1u << 5)) != 0) {
        return false;
    }
    return image_range_valid(patcher->image_base, patcher->image_size,
                             pc, sizeof(uint32_t)) ||
           (patcher->trampoline_mapping &&
            image_range_valid(trampoline, patcher->trampoline_size,
                              pc, sizeof(uint32_t)));
}

static void call_previous_signal_action(int signal_number,
                                        siginfo_t *information,
                                        void *opaque_context) {
    runtime_patcher_t *patcher = g_runtime_active_patcher;
    struct sigaction *previous = signal_number == SIGSYS ?
                                 &patcher->previous_sigsys :
                                 (signal_number == SIGILL ?
                                  &patcher->previous_sigill :
                                  &patcher->previous_sigsegv);

    if (previous->sa_handler == SIG_IGN) {
        return;
    }
    if (previous->sa_handler == SIG_DFL) {
        report_signal("unhandled ", signal_number, information, opaque_context);
        sigaction(signal_number, previous, NULL);
        raise(signal_number);
        return;
    }
    if ((previous->sa_flags & SA_SIGINFO) != 0) {
        previous->sa_sigaction(signal_number, information, opaque_context);
    } else {
        previous->sa_handler(signal_number);
    }
}

static void runtime_signal_handler(int signal_number, siginfo_t *information,
                                   void *opaque_context) {
    runtime_patcher_t *patcher = g_runtime_active_patcher;
    ucontext_t *context = opaque_context;
    bool handled = false;

    if (patcher) {
        if (signal_number == SIGSYS &&
            (information->si_code == SYS_USER_DISPATCH ||
             information->si_code == SYS_SECCOMP)) {
            uint32_t source =
                (uint32_t)(uintptr_t)information->si_call_addr;

            /* ARM seccomp_data records the architectural PC after the SVC,
             * while SUD and QEMU's SIGILL path identify the instruction
             * itself.  Rewind before decoding and patching the call site. */
            if (information->si_code == SYS_SECCOMP && source >= 4u) {
                source -= 4u;
            }
            handled = patch_caught_syscall(
                patcher, source, context);
        } else if (signal_number == SIGILL) {
            handled = patch_caught_syscall(
                patcher, (uint32_t)(uintptr_t)information->si_addr, context);
        } else if (signal_number == SIGSEGV) {
            handled = patch_faulting_peripheral(patcher, information, context);
            if (!handled) {
                handled = patch_faulting_block_transfer(patcher, information,
                                                        context);
            }
            if (!handled) {
                handled = make_heap_page_executable(information, context);
            }
        }
    }
    if (handled) {
        return;
    }
    if (patcher && signal_number == SIGSEGV &&
        patcher->continue_on_segfault && is_runtime_arm_pc(patcher, context)) {
        report_signal("skipping ", signal_number, information, context);
        context->uc_mcontext.arm_pc += sizeof(uint32_t);
        return;
    }
    call_previous_signal_action(signal_number, information, opaque_context);
}

/* ---- static syscall patching ---------------------------------------- */

int runtime_patcher_static_patch_syscalls(runtime_patcher_t *patcher) {
    size_t range_index;
    uint32_t patched = 0;
    uint32_t unregistered = 0;
    uint32_t failed = 0;
    uint32_t exhausted = 0;
    uint32_t skipped_conditional = 0;

    if (!patcher || !patcher->trampoline_mapping || !patcher->patches) {
        errno = EINVAL;
        return -1;
    }

    for (range_index = 0; range_index < patcher->range_count && !exhausted;
         ++range_index) {
        uint32_t cursor = patcher->ranges[range_index].start;
        uint32_t end = cursor + patcher->ranges[range_index].size;

        for (; cursor + 4u <= end; cursor += 4u) {
            uint32_t instruction = *(const volatile uint32_t *)(uintptr_t)cursor;
            ssize_t binding_index;
            pending_patch_t *patch;
            uint32_t *trampoline;

            /* Only unconditional SVCs are rewritten.  Literal pools are full
             * of words whose bits 27..24 look like an SVC (every float
             * constant such as 0x3f800000 does), so requiring cond == AL keeps
             * the scan from ever mistaking data for a call site.  Conditional
             * SVCs still work: they simply keep the fault-driven path. */
            if ((instruction >> 28) != 0xeu) {
                if ((instruction & ARM_SVC_MASK) == ARM_SVC_VALUE) ++skipped_conditional;
                continue;
            }
            if ((instruction & ARM_SVC_MASK) != ARM_SVC_VALUE) continue;

            binding_index = find_syscall(patcher, instruction & 0x00ffffffu);
            if (binding_index < 0) {
                ++unregistered;
                continue;
            }

            patch = reserve_patch(patcher, &trampoline);
            if (!patch) {
                ++exhausted;
                break;
            }
            memset(patch, 0, sizeof(*patch));
            patch->kind = PATCH_SYSCALL;
            patch->source = cursor;
            patch->original = instruction;
            patch->binding_index = (size_t)binding_index;
            if (emit_syscall_trampoline(patcher, patch, trampoline) != 0) {
                ++failed;
                continue;
            }
            ++patched;
        }
    }

    if (exhausted) {
        PS_LOG_ERROR("runtime",
                     "static SVC patch stopped: trampoline pool exhausted after "
                     "%u sites (capacity %zu)",
                     patched, patcher->patch_capacity);
    }
    PS_LOG_INFO("runtime",
                "static SVC patch: %u sites rewritten, %u unregistered immediates, "
                "%u conditional left to the fault path, %u failed",
                patched, unregistered, skipped_conditional, failed);
    return (int)patched;
}

int runtime_patcher_apply(runtime_patcher_t *patcher) {
    uint32_t pool_address;
    uint32_t page = (uint32_t)page_size();
    struct sigaction action;
    size_t total_size = align_up(DYNAMIC_PATCH_LIMIT * TRAMPOLINE_WORDS *
                                 sizeof(uint32_t), page);

    if (!patcher || !patcher->image_mapping || patcher->trampoline_mapping) {
        errno = EINVAL;
        return -1;
    }
    {
        const char *trace = getenv("PRIMESHIM_TRACE_SYSCALLS");
        if (trace && *trace && trace[0] != '0') primeshim_trace_calls = 1;
    }
    pool_address = patcher->image_base + (uint32_t)patcher->image_size;
    if (pool_address > UINT32_MAX - total_size) {
        errno = ERANGE;
        return -1;
    }
    patcher->patches = calloc(DYNAMIC_PATCH_LIMIT, sizeof(*patcher->patches));
    if (!patcher->patches) {
        return -1;
    }
    patcher->patch_capacity = DYNAMIC_PATCH_LIMIT;
    patcher->trampoline_mapping = map_exact_non_destructive(pool_address,
                                                            total_size);
    if (patcher->trampoline_mapping == MAP_FAILED) {
        patcher->trampoline_mapping = NULL;
        return -1;
    }
    patcher->trampoline_size = total_size;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGSYS);
    sigaddset(&action.sa_mask, SIGSEGV);
    sigaddset(&action.sa_mask, SIGILL);
    action.sa_sigaction = runtime_signal_handler;
    /* The guest thread has an alternate stack armed by the JNI layer, so a
     * stack-overflow fault can still reach the handler. */
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSYS, &action, &patcher->previous_sigsys) != 0) {
        return -1;
    }
    if (sigaction(SIGSEGV, &action, &patcher->previous_sigsegv) != 0) {
        sigaction(SIGSYS, &patcher->previous_sigsys, NULL);
        return -1;
    }
    if (sigaction(SIGILL, &action, &patcher->previous_sigill) != 0) {
        sigaction(SIGSYS, &patcher->previous_sigsys, NULL);
        sigaction(SIGSEGV, &patcher->previous_sigsegv, NULL);
        return -1;
    }
    patcher->signals_installed = true;
    g_runtime_active_patcher = patcher;
    PS_LOG_INFO("runtime",
                "armed fault-time patching; %zu-byte trampoline pool at "
                "0x%08x (no firmware scan)", total_size, pool_address);
    return 0;
}

int runtime_patcher_enter_firmware_thread(void) {
    runtime_patcher_t *patcher = g_runtime_active_patcher;

    if (!patcher) {
        errno = EINVAL;
        return -1;
    }
    runtime_syscall_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_INCLUSIVE_ON,
              (unsigned long)patcher->image_base,
              (unsigned long)patcher->image_size,
              &runtime_syscall_selector) == 0) {
        runtime_dispatch_mode = 1u;
        return 0;
    }

    /* Upstream Linux supports SUD only on selected architectures.  A native
     * ARM seccomp filter provides the same fault-time SIGSYS trigger while
     * allowing every syscall whose instruction PC is outside the firmware. */
    {
        uint32_t start = patcher->image_base;
        uint32_t end = start + (uint32_t)patcher->image_size;
        struct sock_filter instructions[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                     offsetof(struct seccomp_data, instruction_pointer) + 4u),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0u, 0u, 4u),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                     offsetof(struct seccomp_data, instruction_pointer)),
            BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, start, 0u, 2u),
            BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, end, 1u, 0u),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog program = {
            .len = (unsigned short)(sizeof(instructions) /
                                    sizeof(instructions[0])),
            .filter = instructions,
        };

        if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) == 0 &&
            prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0) {
            runtime_dispatch_mode = 2u;
            return 0;
        }
    }
    /* QEMU linux-user and kernels without OABI support report these firmware
     * SVC immediates as SIGILL.  That signal carries the exact instruction
     * address and therefore remains fully lazy too. */
    runtime_dispatch_mode = 3u;
    return 0;
}

void runtime_patcher_leave_firmware_thread(void) {
    if (runtime_dispatch_mode == 1u) {
        (void)prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_OFF,
                    0UL, 0UL, NULL);
    }
    runtime_dispatch_mode = 0;
}

uint32_t runtime_patcher_entry(const runtime_patcher_t *patcher) {
    return patcher ? patcher->entry : 0;
}

static uint32_t frame_get_register(const uint32_t *frame, unsigned reg) {
    if (reg < 12u) {
        return frame[reg];
    }
    if (reg == 12u) {
        return frame[13]; /* frame[12] is the trampoline's return LR */
    }
    return 0;
}

static void frame_set_register(uint32_t *frame, unsigned reg, uint32_t value) {
    if (reg < 12u) {
        frame[reg] = value;
    } else if (reg == 12u) {
        frame[13] = value;
    }
}

static uint32_t decode_sdt_offset(uint32_t instruction, const uint32_t *frame,
                                  uint32_t cpsr) {
    uint32_t value;
    unsigned shift;
    unsigned shift_type;

    if ((instruction & (1u << 25)) == 0) {
        return instruction & 0xfffu;
    }
    value = frame_get_register(frame, instruction & 15u);
    shift = (instruction >> 7) & 31u;
    shift_type = (instruction >> 5) & 3u;
    switch (shift_type) {
    case 0:
        return shift ? value << shift : value;
    case 1:
        return shift ? value >> shift : 0;
    case 2:
        if (!shift) {
            return (value & 0x80000000u) ? UINT32_MAX : 0;
        }
        return (uint32_t)((int32_t)value >> shift);
    default:
        return shift ? rotate_right(value, shift) :
               (((cpsr & (1u << 29)) << 2) | (value >> 1));
    }
}

void runtime_peripheral_dispatch(uint32_t *frame, const void *opaque_metadata,
                                 uint32_t cpsr) {
    const peripheral_metadata_t *metadata = opaque_metadata;
    const peripheral_binding_t *binding;
    uint32_t instruction = metadata->instruction;
    unsigned rn = (instruction >> 16) & 15u;
    unsigned rd = (instruction >> 12) & 15u;
    unsigned width = (instruction & (1u << 22)) ? 1u : 4u;
    uint32_t base = frame_get_register(frame, rn);
    uint32_t offset = decode_sdt_offset(instruction, frame, cpsr);
    uint32_t adjusted = (instruction & (1u << 23)) ?
                        base + offset : base - offset;
    bool pre_indexed = (instruction & (1u << 24)) != 0;
    bool writeback = (instruction & (1u << 21)) != 0 || !pre_indexed;
    bool load = (instruction & (1u << 20)) != 0;
    uint32_t address = metadata->address;
    uint32_t value;

    if (!g_runtime_active_patcher ||
        metadata->binding_index >= g_runtime_active_patcher->peripheral_count) {
        return;
    }
    binding = &g_runtime_active_patcher->peripherals[metadata->binding_index];

    if (load) {
        if (!binding->read) {
            return;
        }
        value = binding->read(binding->opaque, address, width);
        if (width == 1u) {
            value &= 0xffu;
        }
        frame_set_register(frame, rd, value);
    } else {
        value = frame_get_register(frame, rd);
        if (!binding->write) {
            return;
        }
        binding->write(binding->opaque, address, width, value);
    }
    if (writeback) {
        frame_set_register(frame, rn, adjusted);
    }
}
