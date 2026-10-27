#include "device.h"

#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MAX_DEVICE_DRIVERS 32
#define MAX_OPEN_DEVICES 64

typedef struct open_device {
    uint32_t handle;
    const primeshim_device_driver_t *driver;
    void *context;
} open_device_t;

static const primeshim_device_driver_t *drivers[MAX_DEVICE_DRIVERS];
static open_device_t open_devices[MAX_OPEN_DEVICES];
static size_t driver_count;
static uint32_t next_handle = 1;
static pthread_mutex_t device_lock = PTHREAD_MUTEX_INITIALIZER;

static bool names_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static const char *path_leaf(const char *path)
{
    const char *leaf = path;

    while (path && *path) {
        if (*path == '\\' || *path == '/') leaf = path + 1;
        ++path;
    }
    return leaf;
}

static const primeshim_device_driver_t *find_driver(const char *guest_path)
{
    const char *leaf = path_leaf(guest_path);
    size_t i;

    if (!leaf || !*leaf) return NULL;
    for (i = 0; i < driver_count; ++i) {
        if (names_equal(leaf, drivers[i]->name)) return drivers[i];
    }
    return NULL;
}

int device_driver_register(const primeshim_device_driver_t *driver)
{
    size_t i;

    if (!driver || !driver->name || !*driver->name || !driver->io_control) {
        errno = EINVAL;
        PS_LOG_ERROR("device", "rejected invalid device driver registration");
        return -1;
    }

    pthread_mutex_lock(&device_lock);
    for (i = 0; i < driver_count; ++i) {
        if (names_equal(driver->name, drivers[i]->name)) {
            pthread_mutex_unlock(&device_lock);
            errno = EEXIST;
            PS_LOG_ERROR("device", "driver %s is already registered",
                         driver->name);
            return -1;
        }
    }
    if (driver_count == MAX_DEVICE_DRIVERS) {
        pthread_mutex_unlock(&device_lock);
        errno = ENOSPC;
        PS_LOG_ERROR("device", "driver registry is full");
        return -1;
    }
    drivers[driver_count++] = driver;
    pthread_mutex_unlock(&device_lock);
    PS_LOG_DEBUG("device", "registered driver %s", driver->name);
    return 0;
}

uint32_t device_open(const char *guest_path)
{
    const primeshim_device_driver_t *driver;
    void *context = NULL;
    size_t slot;
    uint32_t handle;

    if (!guest_path) {
        errno = EINVAL;
        return 0;
    }

    pthread_mutex_lock(&device_lock);
    driver = find_driver(guest_path);
    if (!driver) {
        pthread_mutex_unlock(&device_lock);
        errno = ENODEV;
        PS_LOG_DEBUG("device", "no driver registered for %s", guest_path);
        return 0;
    }
    pthread_mutex_unlock(&device_lock);

    if (driver->open && driver->open(&context) < 0) return 0;

    pthread_mutex_lock(&device_lock);
    for (slot = 0; slot < MAX_OPEN_DEVICES; ++slot) {
        if (!open_devices[slot].handle) break;
    }
    if (slot == MAX_OPEN_DEVICES) {
        pthread_mutex_unlock(&device_lock);
        if (driver->close) driver->close(context);
        errno = EMFILE;
        return 0;
    }
    handle = next_handle++;
    if (!handle) handle = next_handle++;
    open_devices[slot].handle = handle;
    open_devices[slot].driver = driver;
    open_devices[slot].context = context;
    pthread_mutex_unlock(&device_lock);

    PS_LOG_TRACE("device", "opened %s as handle %u", guest_path, handle);
    return handle;
}

int32_t device_io_control(uint32_t handle, uint32_t request,
                          const void *input, uint32_t input_size,
                          void *output, uint32_t output_size,
                          uint32_t *result)
{
    const primeshim_device_driver_t *driver = NULL;
    void *context = NULL;
    int32_t status;
    size_t i;

    if (result) *result = 0;
    pthread_mutex_lock(&device_lock);
    for (i = 0; i < MAX_OPEN_DEVICES; ++i) {
        if (open_devices[i].handle == handle) {
            driver = open_devices[i].driver;
            context = open_devices[i].context;
            break;
        }
    }
    pthread_mutex_unlock(&device_lock);

    if (!driver) {
        errno = EBADF;
        PS_LOG_ERROR("device", "ioctl 0x%x used invalid handle %u", request,
                     handle);
        return 0;
    }

    PS_LOG_TRACE("device", "%s handle=%u ioctl=0x%x in=%u out=%u",
                 driver->name, handle, request, input_size, output_size);
    if (input && input_size && primeshim_log_enabled(PRIMESHIM_LOG_TRACE))
        primeshim_log_hexdump(PRIMESHIM_LOG_TRACE, "device", input,
                              input_size > 128 ? 128 : input_size,
                              "ioctl input (capped at 128 bytes)");

    status = driver->io_control(context, request, input, input_size, output,
                                output_size);
    if (result) *result = (uint32_t)status;
    PS_LOG_TRACE("device", "%s ioctl=0x%x returned %d", driver->name,
                 request, status);
    return status;
}

int device_close(uint32_t handle)
{
    const primeshim_device_driver_t *driver = NULL;
    void *context = NULL;
    size_t i;

    if (!handle) return 0;
    pthread_mutex_lock(&device_lock);
    for (i = 0; i < MAX_OPEN_DEVICES; ++i) {
        if (open_devices[i].handle == handle) {
            driver = open_devices[i].driver;
            context = open_devices[i].context;
            memset(&open_devices[i], 0, sizeof(open_devices[i]));
            break;
        }
    }
    pthread_mutex_unlock(&device_lock);

    if (!driver) {
        errno = EBADF;
        return 0;
    }
    if (driver->close) driver->close(context);
    PS_LOG_TRACE("device", "closed %s handle %u", driver->name, handle);
    return 1;
}
