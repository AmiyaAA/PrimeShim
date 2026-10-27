#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <iconv.h>
#include <stdbool.h>
#include <fnmatch.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <uchar.h>
#include "log.h"
#include "platform.h"



static uint32_t g_next_handle = 1;
static char g_cwds[26][1024] = {0};
static char g_currentDrive = 'A';
static pthread_once_t g_init_flag = PTHREAD_ONCE_INIT;

// 初始化驱动器根目录
__attribute__((constructor)) static void ensure_prime_drive_roots_initialized() {
    for (int i = 0; i < 26; ++i) {
        snprintf(g_cwds[i], sizeof(g_cwds[i]), PRIMESHIM_DATA_DIR "/%c/", 'A' + i);
        mkdir(g_cwds[i], 0777);
    }
}
static char* map_path(const char *win_path) ;
bool ensure_parent_dirs_for_hostpath(const char* hostPath) {
    if (!hostPath || !*hostPath) return false;

    char* pathCopy = strdup(hostPath);
    if (!pathCopy) return false;

    char* parentDir = dirname(pathCopy);
    if (!parentDir) {
        free(pathCopy);
        return false;
    }

    struct stat st;
    if (stat(parentDir, &st) == 0) {
        free(pathCopy);
        return S_ISDIR(st.st_mode);
    }

    char* path = strdup(parentDir);
    if (!path) {
        free(pathCopy);
        return false;
    }

    char* p = path;
    while (*p == '/') p++; // Skip leading slashes

    bool success = true;
    while (true) {
        p = strchr(p, '/');
        if (!p) break;

        *p = '\0';
        if (*path) {
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                success = false;
                break;
            }
        }
        *p++ = '/';
    }

    if (success && mkdir(parentDir, 0755) != 0 && errno != EEXIST) {
        success = false;
    }

    free(path);
    free(pathCopy);
    return success;
}

//--------------INI--------------------
// ====== INI Functions ======

// Helper function to trim whitespace
static char* trim_whitespace(char* str) {
    if (!str) return NULL;
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
    return str;
}

static bool ini_section_is(const char *line, const char *section)
{
    const char *begin;
    const char *end;
    size_t section_len;

    if (!line || !section) return false;
    begin = line;
    while (isspace((unsigned char)*begin)) begin++;
    if (*begin++ != '[') return false;

    end = strchr(begin, ']');
    /* Older PrimeShim builds damaged section headers by replacing the closing
     * bracket with NUL while parsing.  Accept that one legacy form so the
     * next write can normalize it instead of creating a duplicate section. */
    if (!end) end = begin + strlen(begin);
    while (end > begin && isspace((unsigned char)end[-1])) end--;
    while (begin < end && isspace((unsigned char)*begin)) begin++;

    section_len = strlen(section);
    return (size_t)(end - begin) == section_len &&
           memcmp(begin, section, section_len) == 0;
}

static bool ini_key_is(const char *line, const char *key_name)
{
    const char *begin;
    const char *end;
    const char *equals;
    size_t key_len;

    if (!line || !key_name) return false;
    begin = line;
    while (isspace((unsigned char)*begin)) begin++;
    if (!*begin || *begin == ';' || *begin == '#') return false;

    equals = strchr(begin, '=');
    if (!equals) return false;
    end = equals;
    while (end > begin && isspace((unsigned char)end[-1])) end--;

    key_len = strlen(key_name);
    return (size_t)(end - begin) == key_len &&
           memcmp(begin, key_name, key_len) == 0;
}

static uint32_t copy_profile_result(char *out_buf, int size,
                                    const char *value)
{
    size_t value_len = value ? strlen(value) : 0;
    size_t copied = 0;

    if (out_buf && size > 0) {
        copied = value_len;
        if (copied >= (size_t)size) copied = (size_t)size - 1;
        if (copied) memcpy(out_buf, value, copied);
        out_buf[copied] = '\0';
    }
    return (uint32_t)copied;
}

uint32_t SDKLIB__GetPrivateProfileString(const char* appName, const char* keyName, const char* def, char* outBuf, int size, const char* filename) {

    char* hostPath = filename ?  map_path(filename) : NULL;
    PS_LOG_TRACE("filesystem",
                 "GetPrivateProfileString section=%s key=%s default=%s "
                 "size=%d guest=%s host=%s",
                 appName ? appName : "<null>",
                 keyName ? keyName : "<null>", def ? def : "<null>", size,
                 filename ? filename : "<null>",
                 hostPath ? hostPath : "<null>");

    if (!hostPath) {
        return copy_profile_result(outBuf, size, def ? def : "");
    }

    FILE* f = fopen(hostPath, "rb");
    if (!f) {
        free(hostPath);
        return copy_profile_result(outBuf, size, def ? def : "");
    }

    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = malloc(flen + 1);
    if (!content) {
        fclose(f);
        free(hostPath);
        return 0;
    }

    fread(content, 1, flen, f);
    content[flen] = '\0';
    fclose(f);
    free(hostPath);

    bool in_section = false;
    char* line = strtok(content, "\r\n");
    char* found_value = NULL;

    while (line) {
        char* trimmed = trim_whitespace(line);
        size_t len = strlen(trimmed);

        if (len > 0 && trimmed[0] != ';' && trimmed[0] != '#') {
            if (trimmed[0] == '[') {
                in_section = appName && ini_section_is(trimmed, appName);
            } else if (in_section) {
                char* eq = strchr(trimmed, '=');
                if (eq) {
                    *eq = '\0';
                    char* key = trim_whitespace(trimmed);
                    char* value = trim_whitespace(eq + 1);

                    if (keyName && strcmp(key, keyName) == 0) {
                        found_value = strdup(value);
                        break;
                    }
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }

    free(content);

    const char* result = found_value ? found_value : (def ? def : "");
    uint32_t result_len = copy_profile_result(outBuf, size, result);

    free(found_value);
    return result_len;
}

uint32_t SDKLIB__WritePrivateProfileString(const char* appName, const char* keyName, const char* stringToWrite, const char* filename)   {

    char* hostPath = filename ?  map_path(filename) : NULL;

    PS_LOG_TRACE("filesystem",
                 "WritePrivateProfileString section=%s key=%s value=%s "
                 "guest=%s host=%s",
                 appName ? appName : "<null>",
                 keyName ? keyName : "<null>",
                 stringToWrite ? stringToWrite : "<null>",
                 filename ? filename : "<null>",
                 hostPath ? hostPath : "<null>");

    if (!hostPath || !appName) {
        free(hostPath);
        return 0;
    }

    // Ensure parent directories exist
    ensure_parent_dirs_for_hostpath(hostPath);

    // Read existing content or create new file
    char** lines = NULL;
    size_t line_count = 0;
    FILE* f = fopen(hostPath, "rb");

    if (f) {
        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (flen > 0) {
            char* content = malloc(flen + 1);
            if (content) {
                fread(content, 1, flen, f);
                content[flen] = '\0';

                // Parse lines
                char* line = strtok(content, "\n");
                while (line) {
                    // Remove trailing carriage return if present
                    if (line[strlen(line)-1] == '\r') {
                        line[strlen(line)-1] = '\0';
                    }

                    lines = realloc(lines, sizeof(char*) * (line_count + 1));
                    lines[line_count++] = strdup(line);
                    line = strtok(NULL, "\n");
                }
                free(content);
            }
        }
        fclose(f);
    }

    size_t section_start = 0, section_end = 0, key_line = 0;
    bool section_found = false, key_found = false;
    bool in_section = false;

    // Find section and key
    for (size_t i = 0; i < line_count; i++) {
        const char *trimmed = lines[i];
        while (isspace((unsigned char)*trimmed)) trimmed++;
        if (*trimmed == '\0') continue;

        if (*trimmed == '[') {
            if (section_found) {
                section_end = i;
                break;
            }
            in_section = false;
            if (ini_section_is(trimmed, appName)) {
                if (!strchr(trimmed, ']')) {
                    char *normalized = malloc(strlen(appName) + 3u);
                    if (!normalized) goto write_failed;
                    sprintf(normalized, "[%s]", appName);
                    free(lines[i]);
                    lines[i] = normalized;
                }
                section_start = i;
                section_end = line_count;
                section_found = true;
                in_section = true;
            }
        } else if (in_section && keyName) {
            if (ini_key_is(trimmed, keyName)) {
                key_line = i;
                key_found = true;
            }
        }
    }

    // Perform modifications
    if (!keyName && !stringToWrite) {
        // Delete entire section
        if (section_found) {
            for (size_t i = section_start; i < section_end; i++) {
                free(lines[i]);
            }
            memmove(&lines[section_start], &lines[section_end],
                   (line_count - section_end) * sizeof(char*));
            line_count -= (section_end - section_start);
        }
    } else if (keyName && !stringToWrite) {
        // Delete specific key
        if (key_found) {
            free(lines[key_line]);
            memmove(&lines[key_line], &lines[key_line+1],
                   (line_count - key_line - 1) * sizeof(char*));
            line_count--;
        }
    } else if (keyName && stringToWrite) {
        char* new_line = malloc(strlen(keyName) + strlen(stringToWrite) + 2);
        sprintf(new_line, "%s=%s", keyName, stringToWrite);

        if (key_found) {
            // Update existing key
            free(lines[key_line]);
            lines[key_line] = new_line;
        } else if (section_found) {
            // Add to existing section
            lines = realloc(lines, sizeof(char*) * (line_count + 1));
            memmove(&lines[section_end+1], &lines[section_end],
                   (line_count - section_end) * sizeof(char*));
            lines[section_end] = new_line;
            line_count++;
        } else {
            // Create new section
            lines = realloc(lines, sizeof(char*) * (line_count + 3));

            // Add blank line if not empty
            if (line_count > 0) {
                lines[line_count] = strdup("");
                line_count++;
            }

            // Add section header
            char* section_header = malloc(strlen(appName) + 3);
            sprintf(section_header, "[%s]", appName);
            lines[line_count] = section_header;
            line_count++;

            // Add key
            lines[line_count] = new_line;
            line_count++;
        }
    }

    // Write back to file
    f = fopen(hostPath, "wb");
    if (!f) goto write_failed;
    for (size_t i = 0; i < line_count; i++) {
        fprintf(f, "%s\r\n", lines[i]);
        free(lines[i]);
    }
    fclose(f);
    free(lines);
    free(hostPath);
    return 1;

write_failed:
    for (size_t i = 0; i < line_count; i++) {
        free(lines[i]);
    }
    free(lines);
    free(hostPath);
    return 0;
}
//-------------------------------------------
//#define fs_root() "/home/kali/qemuPrime/build/firmware/PrimeU/prime_data"
typedef uint16_t UTF16;
enum fs_attribute_e {
    /** Entry is read only. */
    ATTR_READONLY = 0x1,
    /** Entry is hidden. */
    ATTR_HIDDEN = 0x2,
    /** Entry is a system file/directory. */
    ATTR_SYSTEM = 0x4,
    /** Entry is a directory. */
    ATTR_DIR = 0x10,
    /** Entry is archived. */
    ATTR_ARCHIVE = 0x20,
    ATTR_DEVICE = 0x40,
    /** Entry does not have any other attribute. */
    ATTR_NONE = 0x80,
};
typedef struct {
    /**
     * @brief Current file descriptor.
     */
    void *unk0; // 0x0
    /**
     * @brief Next file descriptor.
     */
    void *unk4; // 0x4
    /** UTF-16-encoded long filename of the entry. */
    UTF16 *filename_lfn; // 0x8
    /** DOS 8.3 filename of the entry. */
    char *filename; // 0xc
    /**
     * @brief Seems to be a mirror of #filename
     * @see filename
     */
    char *filename2_alt; // 0x10
    /** Size of file. */
    size_t size; // 0x14
    /**
     * @brief Modify timestamp.
     * @see FIND_TS_YEAR
     * @see FIND_TS_MONTH
     * @see FIND_TS_DAY
     * @see FIND_TS_HOUR
     * @see FIND_TS_MINUTE
     * @see FIND_TS_SECOND
     */
    unsigned int mtime; // 0x18
    /**
     * @brief Create/birth timestamp.
     * @details The format of this field seems to be similar to mtime, but seems to be corrupted somehow.
     * @see mtime
     */
    unsigned int btime; // 0x1c
    /**
     * @brief Access timestamp.
     * @see mtime
     */
    unsigned int atime; // 0x20
    /** FAT filesystem file attribute mask. */
    unsigned char attrib_mask; // 0x24
    /** FAT filesystem file attributes. */
    unsigned char attrib; // 0x25
} find_context_t;

// Internal state for findfirst/findnext operations.
// We'll store this in the opaque pointers of find_context_t.
typedef struct {
    DIR *dir_handle;
    char *pattern;
    char *search_path;
} linux_find_state_t;

/**
 * @brief Initializes the virtual filesystem root directory.
 * @details Creates the root directory if it doesn't exist.
 */
void fs_init();
// --- Helper Functions ---

// Initializes the VFS root.
/* Resolved on first use: fs_init() runs as a library constructor, before the
 * runtime has changed into its working directory, so resolving there produced
 * paths like "//prime_data" (getcwd() == "/"). */
static char vfs_root_path[PATH_MAX];

const char* fs_root(void) {
    if (vfs_root_path[0]) return vfs_root_path;

    if (PRIMESHIM_DATA_DIR[0] == '/') {
        snprintf(vfs_root_path, sizeof(vfs_root_path), "%s", PRIMESHIM_DATA_DIR);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';
        snprintf(vfs_root_path, sizeof(vfs_root_path), "%s/%s", cwd,
                 PRIMESHIM_DATA_DIR);
    }
    return vfs_root_path;
}

/* Drop the cached path; the working directory is only final once the runtime
 * has started (Android chdir()s into its files directory first). */
void fs_reset_root(void) {
    vfs_root_path[0] = '\0';
}

/* Resolve the root and create it if missing.  Call once the working directory
 * will not change again - never from a library constructor, which runs before
 * the runtime has chosen its directory and would cache a wrong path. */
void fs_prepare_root(void) {
    struct stat st = {0};

    fs_reset_root();
    if (fs_root() && stat(fs_root(), &st) == -1) {
        mkdir(fs_root(), 0755);
        PS_LOG_INFO("filesystem", "created VFS root at %s", fs_root());
    }
}

UTF16* utf8_to_utf16(const char *in) {
    if (!in) return calloc(1, sizeof(UTF16));

    const unsigned char *p = (const unsigned char*)in;
    size_t utf16_len = 0;

    // --- Pass 1: Calculate required length ---
    while (*p) {
        if (*p <= 0x7F) { // 1-byte ASCII
            p += 1;
        } else if ((*p & 0xE0) == 0xC0) { // 2-byte sequence
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) { // 3-byte sequence
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) { // 4-byte sequence (becomes a surrogate pair)
            p += 4;
            utf16_len++; // Extra length for the second surrogate
        } else { // Invalid byte
            p += 1;
        }
        utf16_len++;
    }

    // --- Allocation ---
    UTF16 *out = malloc((utf16_len + 1) * sizeof(UTF16));
    if (!out) return NULL;
    UTF16 *out_ptr = out;
    p = (const unsigned char*)in;

    // --- Pass 2: Perform conversion ---
    while (*p) {
        uint32_t codepoint = 0;
        int len = 0;

        // Decode UTF-8 to a Unicode codepoint
        if (*p <= 0x7F) {
            codepoint = *p; len = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            codepoint = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4;
        } else {
            codepoint = 0xFFFD; len = 1; // Replacement character
        }

        p += len;

        // Encode codepoint to UTF-16
        if (codepoint <= 0xFFFF) {
            *out_ptr++ = (UTF16)codepoint;
        } else {
            codepoint -= 0x10000;
            *out_ptr++ = (UTF16)(0xD800 + (codepoint >> 10));
            *out_ptr++ = (UTF16)(0xDC00 + (codepoint & 0x3FF));
        }
    }

    *out_ptr = 0; // Null terminator
    return out;
}


/**
 * @brief Converts a UTF-16LE string to a newly allocated UTF-8 string.
 *
 * This elegant implementation uses a two-pass approach:
 * 1. Calculate the exact required number of bytes for the UTF-8 string.
 * 2. Allocate the precise amount of memory and perform the conversion.
 *
 * @param in A null-terminated UTF-16LE string.
 * @return A newly allocated, null-terminated UTF-8 string, or NULL on error.
 *         The caller is responsible for freeing the returned memory.
 */
char* utf16_to_utf8(const UTF16 *in) {
    if (!in) return strdup("");

    const UTF16 *p = in;
    size_t utf8_bytes = 0;

    // --- Pass 1: Calculate required bytes ---
    while (*p) {
        uint32_t codepoint = *p;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) { // Surrogate pair
            p++; // Skip low surrogate in length calculation
            utf8_bytes += 4;
        } else if (codepoint <= 0x7F) {
            utf8_bytes += 1;
        } else if (codepoint <= 0x7FF) {
            utf8_bytes += 2;
        } else { // 0x800 to 0xFFFF
            utf8_bytes += 3;
        }
        p++;
    }

    // --- Allocation ---
    char *out = malloc(utf8_bytes + 1);
    if (!out) return NULL;
    char *out_ptr = out;
    p = in;

    // --- Pass 2: Perform conversion ---
    while (*p) {
        uint32_t codepoint = *p++;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) { // High surrogate
            uint32_t low_surrogate = *p++;
            codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low_surrogate - 0xDC00));
        }

        // Encode codepoint to UTF-8
        if (codepoint <= 0x7F) {
            *out_ptr++ = (char)codepoint;
        } else if (codepoint <= 0x7FF) {
            *out_ptr++ = 0xC0 | (codepoint >> 6);
            *out_ptr++ = 0x80 | (codepoint & 0x3F);
        } else if (codepoint <= 0xFFFF) {
            *out_ptr++ = 0xE0 | (codepoint >> 12);
            *out_ptr++ = 0x80 | ((codepoint >> 6) & 0x3F);
            *out_ptr++ = 0x80 | (codepoint & 0x3F);
        } else {
            *out_ptr++ = 0xF0 | (codepoint >> 18);
            *out_ptr++ = 0x80 | ((codepoint >> 12) & 0x3F);
            *out_ptr++ = 0x80 | ((codepoint >> 6) & 0x3F);
            *out_ptr++ = 0x80 | (codepoint & 0x3F);
        }
    }

    *out_ptr = '\0'; // Null terminator
    return out;
}

// Maps a Windows-style path to a Linux path inside our VFS.
// The caller must free the returned string.
// The caller must free the returned string.
static char* map_path(const char *win_path) {
    if (!win_path) return NULL;

    // Calculate required buffer size
    size_t vfs_root_len = strlen(fs_root());
    size_t win_path_len = strlen(win_path);
    size_t buffer_size = vfs_root_len + win_path_len + 10; // Extra space for modifications
    char *linux_path = malloc(buffer_size);
    if (!linux_path) return NULL;

    const char *p = win_path;
    size_t offset = 0; // Track current position in linux_path

    // Handle \\?\ prefix
    if (strncmp(p, "\\\\?\\", 4) == 0) {
        p += 4;
    }

    // Handle UNC paths \\server\share -> /vfs/UNC/server/share
    if (strncmp(p, "\\\\", 2) == 0) {
        const char *unc_start = p + 2; // Skip initial \\/
        const char *slash = strchr(unc_start, '\\');
        if (slash) {
            const char *share_start = slash + 1;
            const char *share_slash = strchr(share_start, '\\');
            if (share_slash) {
                // Format: /vfs/UNC/server/share/rest
                offset += snprintf(linux_path + offset, buffer_size - offset, "%s/UNC/", fs_root());
                // Copy server name
                strncat(linux_path + offset, unc_start, slash - unc_start);
                offset += slash - unc_start;
                // Copy share name
                offset += snprintf(linux_path + offset, buffer_size - offset, "/");
                strncat(linux_path + offset, share_start, share_slash - share_start);
                offset += share_slash - share_start;
                p = share_slash; // Set p to the rest of the path
            } else {
                // Format: /vfs/UNC/server/share
                offset += snprintf(linux_path + offset, buffer_size - offset, "%s/UNC/", fs_root());
                strncat(linux_path + offset, unc_start, buffer_size - offset);
                offset += strlen(unc_start);
                p += strlen(p); // No rest path
            }
        } else {
            // Invalid UNC path, treat as relative
            linux_path[offset] = '\0';
        }
    }
    // Handle drive letters (e.g., C:\ or C:/)
    else if (win_path_len >= 2 && isalpha(p[0]) && p[1] == ':') {
        char drive_letter = toupper(p[0]);
        offset += snprintf(linux_path, buffer_size, "%s/%c", fs_root(), drive_letter);
        p += 2; // Skip drive letter and colon
        // If next character is slash, skip it (root case)
        if (*p == '\\' || *p == '/') {
            p++;
        }
    }
    // Handle absolute paths starting with slash (root of current drive)
    else if (win_path_len >= 1 && (p[0] == '\\' || p[0] == '/')) {
        // Map to current drive root (assuming default drive, e.g., C)
        offset += snprintf(linux_path, buffer_size, "%s/C", fs_root());
        p++; // Skip the leading slash
    }
    // Relative path
    else {
        linux_path[offset] = '\0'; // Start empty
    }

    // Append the rest of the path
    if (*p) {
        // Add separator if needed (if not empty and last char isn't slash)
        if (offset > 0 && linux_path[offset - 1] != '/') {
            offset += snprintf(linux_path + offset, buffer_size - offset, "/");
        }
        strncpy(linux_path + offset, p, buffer_size - offset);
        offset += strlen(p);
    }

    // Convert all backslashes to forward slashes
    for (char *c = linux_path; *c; c++) {
        if (*c == '\\') *c = '/';
    }

    return linux_path;
}


// Generates a simple 8.3 filename from a long filename.
static void generate_8_3_name(const char *lfn, char *sfn) {
    char basename[256];
    char extension[256];
    const char *dot = strrchr(lfn, '.');

    if (dot && dot != lfn) {
        strncpy(basename, lfn, dot - lfn);
        basename[dot - lfn] = '\0';
        strcpy(extension, dot + 1);
    } else {
        strcpy(basename, lfn);
        extension[0] = '\0';
    }

    char sfn_base[9] = {0};
    char sfn_ext[4] = {0};
    int j = 0;

    // Base name
    for (int i = 0; basename[i] && j < 8; ++i) {
        char c = toupper(basename[i]);
        if (isalnum(c)) {
            sfn_base[j++] = c;
        }
    }
    if (strlen(basename) > 8) {
        sfn_base[6] = '~';
        sfn_base[7] = '1';
    }

    // Extension
    j = 0;
    for (int i = 0; extension[i] && j < 3; ++i) {
        char c = toupper(extension[i]);
        if (isalnum(c)) {
            sfn_ext[j++] = c;
        }
    }

    if (strlen(sfn_ext) > 0) {
        sprintf(sfn, "%-8s.%-3s", sfn_base, sfn_ext);
    } else {
        sprintf(sfn, "%-8s", sfn_base);
    }
    // Trim trailing spaces
    char *end = sfn + strlen(sfn) - 1;
    while (end > sfn && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

// Converts time_t to FAT timestamp format
static unsigned int time_t_to_fat_ts(time_t t) {
    struct tm *local = localtime(&t);
    if (!local) return 0;

    unsigned int year = (local->tm_year + 1900 - 1980);
    unsigned int month = local->tm_mon + 1;
    unsigned int day = local->tm_mday;
    unsigned int hour = local->tm_hour;
    unsigned int min = local->tm_min;
    unsigned int sec = local->tm_sec;

    return (year << 25) | (month << 21) | (day << 16) |
           (hour << 11) | (min << 5) | (sec / 2); // FAT stores seconds/2
}

// --- `_a*` (ANSI/8.3) Implementations ---

short SDKLIB__achdir(const char *path) {
    PS_LOG_TRACE("filesystem", "chdir guest path %s",
                 path ? path : "<null>");
    char *linux_path = map_path(path);
    if (!linux_path) {

        return -1;
    }
    int ret = chdir(linux_path);
    free(linux_path);
    return (ret == 0) ? 0 : -1;
}

int SDKLIB__amkdir(char *path) {
    PS_LOG_TRACE("filesystem", "mkdir guest path %s",
                 path ? path : "<null>");
    char *linux_path = map_path(path);
    if (!linux_path) {
        return -1;
    }

    // Ensure parent directories exist.
    char *p = linux_path;
    if (p[0] == '/') p++;
    while ((p = strchr(p, '/'))) {
        *p = '\0';
        mkdir(linux_path, 0755);
        *p = '/';
        p++;
    }

    int ret = mkdir(linux_path, 0755);
    free(linux_path);
    return (ret == 0 || errno == EEXIST) ? 0 : -1;
}

int SDKLIB__armdir(char *path) {
    char *linux_path = map_path(path);
    if (!linux_path) {
        return -1;
    }
    int ret = rmdir(linux_path);
    free(linux_path);
    return (ret == 0) ? 0 : -1;
}

short SDKLIB__afindnext(find_context_t *ctx); // Forward declaration

short SDKLIB__afindfirst(const char *fnmatch_path, find_context_t *ctx, int attrib_mask) {
    PS_LOG_TRACE("filesystem", "findfirst pattern=%s attributes=0x%x",
                 fnmatch_path ? fnmatch_path : "<null>", attrib_mask);
        memset(ctx, 0, sizeof(find_context_t));
    // FIX: Correctly separate directory path from glob pattern
    char *win_path_copy = strdup(fnmatch_path);
    char *win_dir_part_str;
    char *pattern_str;

    char *last_slash = strrchr(win_path_copy, '\\');
    char *last_fwd_slash = strrchr(win_path_copy, '/');
    if (last_fwd_slash > last_slash) last_slash = last_fwd_slash;

    if (last_slash) {
        // Path has a directory component, e.g., "C:\DATA\*.*"
        pattern_str = strdup(last_slash + 1);
        *(last_slash + 1) = '\0'; // Truncate to get the directory part
        win_dir_part_str = win_path_copy;
    } else {
        // Path is just a pattern, e.g., "*.*"
        pattern_str = strdup(win_path_copy);
        win_dir_part_str = strdup("."); // Use current directory
    }

    char *search_path = map_path(win_dir_part_str);
    free(win_path_copy);
    if (win_dir_part_str[0] == '.') free(win_dir_part_str);

    if (!search_path) {
        free(pattern_str);
        return -1;
    }

    DIR *dir = opendir(search_path);
    if (!dir) {
        PS_LOG_ERRNO_DEBUG("filesystem", errno, "cannot open directory %s",
                           search_path);
        free(pattern_str);
        free(search_path);
        return -1;
    }

    linux_find_state_t *state = malloc(sizeof(linux_find_state_t));
    state->dir_handle = dir;
    state->pattern = pattern_str;
    state->search_path = search_path;

    ctx->unk0 = state;
    ctx->unk4 = (void*)0xdeadbeef;


    // Find the first actual match
    return SDKLIB__afindnext(ctx);
}
short SDKLIB__afindnext(find_context_t *ctx) {
    PS_LOG_TRACE("filesystem", "findnext context=%p", (void *)ctx);
    linux_find_state_t *state = (linux_find_state_t *)ctx->unk0;
    if (!state || !state->dir_handle) {
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(state->dir_handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Match against pattern
        if (fnmatch(state->pattern, entry->d_name, FNM_PATHNAME) == 0) {

            // Use libgen.h to safely combine paths
            char full_entry_path[PATH_MAX];
            if (snprintf(full_entry_path, sizeof(full_entry_path), "%s/%s", state->search_path, entry->d_name) >= sizeof(full_entry_path)) {
                continue;
            }

            // Normalize path to handle any redundant slashes
            char resolved_path[PATH_MAX];
            if (realpath(full_entry_path, resolved_path) == NULL) {
                continue;
            }

            struct stat st;
            if (stat(resolved_path, &st) != 0) {
                continue;
            }

            unsigned char attrib = 0;
            if (S_ISDIR(st.st_mode)) attrib |= ATTR_DIR;
            if (!(st.st_mode & S_IWUSR)) attrib |= ATTR_READONLY;


            // Free previous results if any
            free(ctx->filename_lfn);
            free(ctx->filename);
            free(ctx->filename2_alt);

            // Populate context
            ctx->filename_lfn = utf8_to_utf16(entry->d_name);
            ctx->filename = strdup(entry->d_name);
            ctx->filename2_alt = strdup(entry->d_name);

            char sfn[13];
            generate_8_3_name(entry->d_name, sfn);
            // Overwrite `filename` with the generated 8.3 name
            free(ctx->filename);
            ctx->filename = strdup(sfn);

            ctx->size = st.st_size;
            ctx->mtime = time_t_to_fat_ts(st.st_mtime);
            ctx->atime = time_t_to_fat_ts(st.st_atime);
            ctx->btime = time_t_to_fat_ts(st.st_ctime); // ctime is closest to birth time
            ctx->attrib = attrib;

            PS_LOG_TRACE("filesystem",
                         "find match path=%s resolved=%s name=%s short=%s "
                         "size=%ld attributes=0x%02x",
                         full_entry_path, resolved_path, entry->d_name, sfn,
                         (long)st.st_size, attrib);

            return 0; // Found a match
        }
    }

    PS_LOG_TRACE("filesystem", "findnext reached end of directory");
    return -1; // No more matches
}

bool SDKLIB__aremove(const char *pathname) {
    char *linux_path = map_path(pathname);
    if (!linux_path) {
        return false;
    }
    int result = remove(linux_path);
    free(linux_path);
    return (result == 0); // remove() returns 0 on success
}
// --- `_w*` (UTF-16/LFN) Implementations ---

short SDKLIB__wchdir(const UTF16 *path) {
    char *utf8_path = utf16_to_utf8(path);
    if (!utf8_path) return -1;
    short ret = SDKLIB__achdir(utf8_path);
    free(utf8_path);
    return ret;
}

int SDKLIB__wmkdir(UTF16 *path) {
    char *utf8_path = utf16_to_utf8(path);
    if (!utf8_path) return -1;
    int ret = SDKLIB__amkdir(utf8_path);
    free(utf8_path);
    return ret;
}

int SDKLIB__wrmdir(UTF16 *path) {
    char *utf8_path = utf16_to_utf8(path);
    if (!utf8_path) return -1;
    int ret = SDKLIB__armdir(utf8_path);
    free(utf8_path);
    return ret;
}

short SDKLIB__wfindfirst(const UTF16 *fnmatch, find_context_t *ctx, int attrib_mask) {
    char *utf8_fnmatch = utf16_to_utf8(fnmatch);
    if (!utf8_fnmatch) return -1;
    short ret = SDKLIB__afindfirst(utf8_fnmatch, ctx, attrib_mask);
    free(utf8_fnmatch);
    return ret;
}

short SDKLIB__wfindnext(find_context_t *ctx) {
    // _wfindnext and _afindnext are interchangeable in this implementation
    // as the internal state is based on UTF-8 anyway.
    return SDKLIB__afindnext(ctx);
}
bool SDKLIB___wremove(const UTF16 *pathname) {
    char *utf8_path = utf16_to_utf8(pathname);
    if (!utf8_path) {
        return false;
    }
    bool result = SDKLIB__aremove(utf8_path);
    free(utf8_path);
    return result;
}
// --- Common Implementation ---

int SDKLIB__findclose(find_context_t *ctx) {
if(!ctx){
PS_LOG_ERROR("filesystem", "findclose called with a null context");
return -1;
}
    if (ctx && ctx->unk0) {
        linux_find_state_t *state = (linux_find_state_t *)ctx->unk0;
        closedir(state->dir_handle);
        free(state->pattern);
        free(state->search_path);
        free(state);
        ctx->unk0 = NULL;
    }
    // Free the strings allocated in the context
    free(ctx->filename_lfn);
    free(ctx->filename);
    free(ctx->filename2_alt);
    memset(ctx, 0, sizeof(find_context_t));
    return 0;
}
//-------------------------------------------------
enum sys_seek_whence_e {
    /** Seek from the beginning of file. */
    _SYS_SEEK_SET = 0,
    /** Seek from current offset. */
    _SYS_SEEK_CUR,
    /** Seek from the end of file. */
    _SYS_SEEK_END,
};

// =========================================================
// === NEW IMPLEMENTATIONS FOR file.h FUNCTIONS          ===
// =========================================================



FILE *SDKLIB__afopen(const char *pathname, const char *mode) {
    PS_LOG_DEBUG("fs", "afopen %s", pathname ? pathname : "<null>");

    char *linux_path = map_path(pathname);
    if (!linux_path) {
        PS_LOG_DEBUG("fs", "afopen %s -> %p", pathname ? pathname : "<null>", (void*)(NULL));
    return NULL;
    }

    FILE *fp = fopen(linux_path, mode);

    PS_LOG_TRACE("filesystem", "fopen path=%s mode=%s result=%p",
                 pathname ? pathname : "<null>", mode ? mode : "<null>",
                 (void *)fp);
    free(linux_path);

    // We cast the standard FILE* to our opaque FILE*

    PS_LOG_DEBUG("fs", "afopen %s -> %p", pathname ? pathname : "<null>", (void *)fp);
    return (FILE *)fp;
}

FILE *SDKLIB___wfopen(const UTF16 *pathname, const UTF16 *mode) {

    char *utf8_path = utf16_to_utf8(pathname);
    char *utf8_mode = utf16_to_utf8(mode);

    if (!utf8_path || !utf8_mode) {
        free(utf8_path);
        free(utf8_mode);
        return NULL;
    }
      // Reuse the ANSI implementation
    FILE *fd = SDKLIB__afopen(utf8_path, utf8_mode);
    PS_LOG_TRACE("filesystem", "wfopen path=%s mode=%s result=%p", utf8_path,
                 utf8_mode, (void *)fd);
    free(utf8_path);
    free(utf8_mode);

    return fd;
}

size_t SDKLIB__fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    PS_LOG_DEBUG("fs", "fread stream=%p size=%u nmemb=%u", (void*)stream, (unsigned)size, (unsigned)nmemb);

    // Cast our opaque pointer back to the underlying FILE*
    size_t result = fread(ptr, size, nmemb, (FILE *)stream);

    return result;
}

size_t SDKLIB__fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    PS_LOG_TRACE("filesystem", "fwrite source=%p bytes=%zu", ptr,
                 size * nmemb);
    size_t result = fwrite(ptr, size, nmemb, (FILE *)stream);

    return result;
}

int SDKLIB___fseek(FILE *stream, long offset, int whence) {
    int linux_whence;
    switch (whence) {
        case _SYS_SEEK_SET:
            linux_whence = SEEK_SET;
            break;
        case _SYS_SEEK_CUR:
            linux_whence = SEEK_CUR;
            break;
        case _SYS_SEEK_END:
            linux_whence = SEEK_END;
            break;
        default:
            return -1; // Invalid whence value
    }

    int result = fseek((FILE *)stream, offset, linux_whence);
    return result;
}

long _ftell(FILE *stream) {
    long result = ftell((FILE *)stream);
    return result;
}

int __fflush(FILE *stream) {
    int result = fflush((FILE *)stream);
    return result;
}

int SDKLIB__fclose(FILE *stream) {
    int result = fclose((FILE *)stream);
    return result;
}
//------------------------------------loader rel----
size_t SDKLIB__filesize(FILE *file) {

    if (file == NULL) {
        errno = EINVAL; // Invalid argument
        return (size_t)-1;
    }
    int fd = fileno(file);
    if (fd == -1) {
        return (size_t)-1;
    }
    struct stat stat_buf;
    if (fstat(fd, &stat_buf) == -1) {
        return (size_t)-1;
    }

    return (size_t)stat_buf.st_size;
}
// Maximum lengths for DOS 8.3 components
#define FNSPLIT_DOS_PATHNAME_MAX 260
#define FNSPLIT_DOS_DRIVE_MAX    3   // e.g., "C:"
#define FNSPLIT_DOS_DIRNAME_MAX  256
#define FNSPLIT_DOS_BASENAME_MAX 9   // 8 chars + null
#define FNSPLIT_DOS_SUFFIX_MAX   4   // 3 chars + null

// Bitmask flags for split/merge results
#define FNSPLIT_DRIVE     0x01
#define FNSPLIT_DIR       0x02
#define FNSPLIT_FILENAME  0x04
#define FNSPLIT_SUFFIX    0x08
// --- End of assumed definitions ---



// --- Provided Helper Function ---
void ucscpy(uint16_t *dest, const uint16_t *src) {
    while ((*dest++ = *src++) != 0);
}


/*
 * ============================================================================
 * ANSI (char*) IMPLEMENTATIONS
 * The core logic resides here.
 * ============================================================================
 */

int SDKLIB__afnsplit(const char *pathname, char *drive, char *dirname, char *basename, char *suffix) {
    // This implementation is mostly correct, no major changes needed.
    // We'll keep it as is to focus on the wrapper bugs.
    if (!pathname) {
        return 0;
    }
    if (drive) *drive = '\0';
    if (dirname) *dirname = '\0';
    if (basename) *basename = '\0';
    if (suffix) *suffix = '\0';

    int result = 0;
    const char *p = pathname;
    const char *last_slash = NULL;
    const char *last_dot = NULL;
    const char *iter;

    for (iter = pathname; *iter; ++iter) {
        if (*iter == '\\' || *iter == '/') {
            last_slash = iter;
        } else if (*iter == '.') {
            last_dot = iter;
        }
    }

    if (p[0] && p[1] == ':') {
        if (drive) { strncpy(drive, p, 2); drive[2] = '\0'; }
        result |= FNSPLIT_DRIVE;
        p += 2;
    }

    if (last_dot && (!last_slash || last_dot > last_slash)) {
        if (suffix) {
            strncpy(suffix, last_dot, FNSPLIT_DOS_SUFFIX_MAX - 1);
            suffix[FNSPLIT_DOS_SUFFIX_MAX - 1] = '\0';
        }
        result |= FNSPLIT_SUFFIX;
        if (basename) {
            const char* start = last_slash ? last_slash + 1 : p;
            size_t len = last_dot - start;
            if (len >= FNSPLIT_DOS_BASENAME_MAX) len = FNSPLIT_DOS_BASENAME_MAX - 1;
            strncpy(basename, start, len);
            basename[len] = '\0';
        }
        result |= FNSPLIT_FILENAME;
    } else {
        if (basename) {
            const char* start = last_slash ? last_slash + 1 : p;
            strncpy(basename, start, FNSPLIT_DOS_BASENAME_MAX - 1);
            basename[FNSPLIT_DOS_BASENAME_MAX - 1] = '\0';
        }
        if (basename && *basename) {
            result |= FNSPLIT_FILENAME;
        }
    }

    if (last_slash) {
        if (dirname) {
            size_t len = (last_slash - p) + 1;
            if (len >= FNSPLIT_DOS_DIRNAME_MAX) len = FNSPLIT_DOS_DIRNAME_MAX - 1;
            strncpy(dirname, p, len);
            dirname[len] = '\0';
        }
        result |= FNSPLIT_DIR;
    }
    return result;
}


/**
 * @brief Build a DOS 8.3 pathname from parts. (IMPROVED VERSION)
 */
int SDKLIB__afnmerge(char *pathname, const char *drive, const char *dirname, const char *basename, const char *suffix) {
    if (!pathname) {
        return 0;
    }

    *pathname = '\0';
    int result = 0;
    char *p = pathname;
    const char *end = pathname + FNSPLIT_DOS_PATHNAME_MAX - 1; // Pointer to last writable char

    // Helper macro to append a string safely
    #define APPEND_STR(src, flag) \
        if (src && *src) { \
            const char *s = src; \
            while (*s && p < end) { *p++ = *s++; } \
            result |= (flag); \
        }

    // 1. Add drive
    if (drive && *drive) {
        const char *s = drive;
        while (*s && p < end) { *p++ = *s++; }
        // Add colon if not present and there's space
        if (p > pathname && *(p-1) != ':' && p < end) {
            *p++ = ':';
        }
        result |= FNSPLIT_DRIVE;
    }

    // 2. Add directory, basename, and suffix
    APPEND_STR(dirname, FNSPLIT_DIR);
    APPEND_STR(basename, FNSPLIT_FILENAME);
    APPEND_STR(suffix, FNSPLIT_SUFFIX);

    *p = '\0'; // Null-terminate the final string

    return result;
}


/*
 * ============================================================================
 * Wide-Char (UTF16*) IMPLEMENTATIONS
 * These are wrappers around the ANSI versions.
 * The logical flaws are corrected here.
 * ============================================================================
 */

int SDKLIB__wfnsplit(const UTF16 *pathname, UTF16 *drive, UTF16 *dirname, UTF16 *basename, UTF16 *suffix) {
    char *utf8_path = utf16_to_utf8(pathname);
    if (!utf8_path) {
        return 0; // Return 0 as per original on null input
    }

    char *utf8_drive = drive ? malloc(FNSPLIT_DOS_DRIVE_MAX) : NULL;
    char *utf8_dirname = dirname ? malloc(FNSPLIT_DOS_DIRNAME_MAX) : NULL;
    char *utf8_basename = basename ? malloc(FNSPLIT_DOS_BASENAME_MAX) : NULL;
    char *utf8_suffix = suffix ? malloc(FNSPLIT_DOS_SUFFIX_MAX) : NULL;

    // Call the ANSI implementation
    int result = SDKLIB__afnsplit(utf8_path, utf8_drive, utf8_dirname, utf8_basename, utf8_suffix);

    // BUG FIX: The conversion back to UTF-16 must happen on SUCCESS (result != 0),
    // not on failure (result == 0). It's simplest to just perform the conversion
    // if the buffer was allocated, as _afnsplit will leave the string empty if
    // a component was not found.

    if (drive && utf8_drive) {
        UTF16 *wide_drive = utf8_to_utf16(utf8_drive);
        if (wide_drive) { ucscpy(drive, wide_drive); free(wide_drive); }
    }
    if (dirname && utf8_dirname) {
        UTF16 *wide_dirname = utf8_to_utf16(utf8_dirname);
        if (wide_dirname) { ucscpy(dirname, wide_dirname); free(wide_dirname); }
    }
    if (basename && utf8_basename) {
        UTF16 *wide_basename = utf8_to_utf16(utf8_basename);
        if (wide_basename) { ucscpy(basename, wide_basename); free(wide_basename); }
    }
    if (suffix && utf8_suffix) {
        UTF16 *wide_suffix = utf8_to_utf16(utf8_suffix);
        if (wide_suffix) { ucscpy(suffix, wide_suffix); free(wide_suffix); }
    }

    // Cleanup
    free(utf8_path);
    free(utf8_drive);
    free(utf8_dirname);
    free(utf8_basename);
    free(utf8_suffix);

    return result;
}

int SDKLIB__wfnmerge(UTF16 *pathname, const UTF16 *drive, const UTF16 *dirname, const UTF16 *basename, const UTF16 *suffix) {
    if (!pathname) {
        return 0;
    }

    // Convert UTF16 components to UTF8
    char *utf8_drive = drive ? utf16_to_utf8(drive) : NULL;
    char *utf8_dirname = dirname ? utf16_to_utf8(dirname) : NULL;
    char *utf8_basename = basename ? utf16_to_utf8(basename) : NULL;
    char *utf8_suffix = suffix ? utf16_to_utf8(suffix) : NULL;
    char *utf8_path = malloc(FNSPLIT_DOS_PATHNAME_MAX);

    if (!utf8_path) { // Check malloc for path buffer immediately
        free(utf8_drive); free(utf8_dirname); free(utf8_basename); free(utf8_suffix);
        return -1; // Indicate memory allocation failure
    }

    // Call the ANSI implementation
    int result = SDKLIB__afnmerge(utf8_path, utf8_drive, utf8_dirname, utf8_basename, utf8_suffix);

    // BUG FIX: The conversion back to UTF-16 must happen on SUCCESS.
    // We check if any part was merged (result != 0).
    if (result != 0) {
        UTF16 *wide_path = utf8_to_utf16(utf8_path);
        if (wide_path) {
            ucscpy(pathname, wide_path);
            free(wide_path);
        } else {
            // Conversion failed, indicate an error
            *pathname = 0;
            result = -1; // Override result to indicate this new error
        }
    } else {
        // Nothing was merged, ensure output is an empty string
        *pathname = 0;
    }

    // Cleanup
    free(utf8_drive);
    free(utf8_dirname);
    free(utf8_basename);
    free(utf8_suffix);
    free(utf8_path);

    return result;
}



#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

typedef struct loader_file_descriptor_s {
    FILE *cart;
    void *parent_fd;
    size_t subfile_base;
    size_t size;
    size_t subfile_offset;
    short unk_0x14;
    short unk_0x16;
    unsigned int unk_0x18;
    unsigned int unk_0x1c;
} loader_file_descriptor_t;

loader_file_descriptor_t *SDKLIB__OpenFile(const char *pathname, const char *mode) {
    PS_LOG_DEBUG("fs", "open %s", pathname ? pathname : "<null>");
    PS_LOG_TRACE("filesystem", "OpenFile path=%s mode=%s",
                 pathname ? pathname : "<null>", mode ? mode : "<null>");

    FILE *cart = SDKLIB__afopen(pathname, mode);
    if (!cart) return NULL;

    loader_file_descriptor_t *fd = calloc(1, sizeof(loader_file_descriptor_t));
    if (!fd) {
        fclose(cart);
        errno = ENOMEM;
        return NULL;
    }

    fd->cart = cart;
    fd->parent_fd = NULL;
    fd->subfile_base = 0;
    fd->subfile_offset = 0;
    fd->unk_0x14 = 0;
    fd->unk_0x16 = 0;
    fd->unk_0x18 = 0;
    fd->unk_0x1c = 0;

    fseek(cart, 0, SEEK_END);
    fd->size = ftell(cart);
    fseek(cart, 0, SEEK_SET);

    PS_LOG_TRACE("filesystem", "OpenFile result=%p size=%zu", (void *)fd,
                 fd->size);
    return fd;
}

loader_file_descriptor_t *SDKLIB__OpenFileW(const UTF16 *pathname, const UTF16 *mode) {
    // Convert UTF16 to UTF8
    char *utf8_path = utf16_to_utf8(pathname);
    char *utf8_mode = utf16_to_utf8(mode);

    if (!utf8_path || !utf8_mode) {
        free(utf8_path);
        free(utf8_mode);
        return NULL;
    }

    // Reuse the ANSI implementation
    loader_file_descriptor_t *fd = SDKLIB__OpenFile(utf8_path, utf8_mode);

    free(utf8_path);
    free(utf8_mode);

    return fd;
}

size_t SDKLIB__FileSize(loader_file_descriptor_t *stream) {
    if (!stream) {
        errno = EINVAL;
        return (size_t)-1;
    }
    PS_LOG_TRACE("filesystem", "FileSize stream=%p result=%zu",
                 (void *)stream, stream->size);
    return stream->size;
}

loader_file_descriptor_t *SDKLIB__OpenSubFile(loader_file_descriptor_t *parent, size_t base, size_t max_size) {
    if (!parent) {
        errno = EINVAL;
        return NULL;
    }
    PS_LOG_TRACE("filesystem", "OpenSubFile parent=%p base=%zu size=%zu",
                 (void *)parent, base, max_size);

    if (base + max_size > parent->size) {
        PS_LOG_ERROR("filesystem",
                     "sub-file [%zu, %zu) exceeds parent size %zu", base,
                     base + max_size, parent->size);
        errno = EINVAL;
        return NULL;
    }

    loader_file_descriptor_t *fd = calloc(1, sizeof(loader_file_descriptor_t));
    if (!fd) {
        errno = ENOMEM;
        return NULL;
    }

    fd->cart = parent->cart;
    fd->parent_fd = parent;
    fd->subfile_base = parent->subfile_base + base;
    fd->size = max_size;
    fd->subfile_offset = 0;
    fd->unk_0x14 = 1;
    fd->unk_0x16 = 0;
    fd->unk_0x18 = 0;
    fd->unk_0x1c = 0;

    PS_LOG_TRACE("filesystem", "OpenSubFile result=%p", (void *)fd);
    return fd;
}

int SDKLIB__CloseFile(loader_file_descriptor_t *stream) {
    if (!stream) return 0;

    PS_LOG_TRACE("filesystem", "CloseFile stream=%p", (void *)stream);

    if (!stream->parent_fd) {
        fclose(stream->cart);
    }
    free(stream);

    return 0;
}

int SDKLIB__FseekFile(loader_file_descriptor_t *stream, size_t offset, int whence) {
    PS_LOG_DEBUG("fs", "seek stream=%p offset=%zu whence=%d", (void *)stream,
                 offset, whence);
    if (!stream) {
        errno = EINVAL;
        return -1;
    }

    size_t new_offset;
    switch (whence) {
        case _SYS_SEEK_SET: new_offset = offset; break;
        case _SYS_SEEK_CUR: new_offset = stream->subfile_offset + offset; break;
        case _SYS_SEEK_END: new_offset = stream->size + offset; break;
        default: errno = EINVAL; return -1;
    }

    if (new_offset > stream->size) {
        errno = EINVAL;
        return -1;
    }
    stream->subfile_offset = new_offset;
    PS_LOG_TRACE("filesystem",
                 "FseekFile stream=%p offset=%zu whence=%d new_offset=%zu",
                 (void *)stream, offset, whence, new_offset);
    return 0;
}

size_t SDKLIB__ReadFile(loader_file_descriptor_t *stream, void *ptr, size_t size) {
    if (!stream || !ptr) {
        errno = EINVAL;
        return 0;
    }

    size_t bytes_remaining = stream->size - stream->subfile_offset;
    if (bytes_remaining == 0) return 0;

    size_t to_read = MIN(size, bytes_remaining);
    size_t absolute_offset = stream->subfile_base + stream->subfile_offset;

    if (fseek(stream->cart, absolute_offset, SEEK_SET) != 0) {
        return 0;
    }

    size_t bytes_read = fread(ptr, 1, to_read, stream->cart);
    stream->subfile_offset += bytes_read;

    PS_LOG_TRACE("filesystem",
                 "ReadFile stream=%p output=%p requested=%zu read=%zu "
                 "absolute_offset=%zu",
                 (void *)stream, ptr, size, bytes_read, absolute_offset);
    return bytes_read;
}





/**
 * @brief Basic info on a loaded applet.
 */
typedef struct loader_applet_info_s {
    /**
     * @brief Magic bytes of ROM spec.
     */
    unsigned short rom_magic;
    /**
     * @brief Unknown, seems unused.
     */
    unsigned short unused_0x2;
    /**
     * @brief Type field inside the ROM spec.
     */
    unsigned short rom_type;
    /**
     * @brief Unknown, seems unused.
     */
    unsigned short unused_0x4;
    /**
     * @brief Checksum of the executable file.
     * @details On Besta RTOS Arm, this seems to be the checksum of the PE file.
     */
    unsigned int exe_checksum;
    /**
     * @brief File size of the executable.
     */
    size_t exe_raw_size;
    /**
     * @brief Unknown, seems unused.
     */
    unsigned char unused_0x10[0x74];
} loader_applet_info_t;
int SDKLIB_GetApplicationHeadInfoA(const char* pathname,loader_applet_info_t *info){
PS_LOG_TRACE("filesystem", "GetApplicationHeadInfo path=%s output=%p",
             pathname ? pathname : "<null>", (void *)info);
return 0;
}
