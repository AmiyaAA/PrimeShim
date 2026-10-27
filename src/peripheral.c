#include "peripheral.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>

#define MAX_PERIPHERAL_DRIVERS 16

typedef struct registered_driver {
    const primeshim_peripheral_driver_t *driver;
    bool active;
} registered_driver_t;

static registered_driver_t drivers[MAX_PERIPHERAL_DRIVERS];
static unsigned int driver_count;
static uint32_t active_classes;
static bool bus_started;

int peripheral_driver_register(const primeshim_peripheral_driver_t *driver)
{
    if (!driver || !driver->name || !driver->classes || !driver->start) {
        errno = EINVAL;
        PS_LOG_ERROR("peripheral", "rejected invalid driver registration");
        return -1;
    }
    if (driver_count >= MAX_PERIPHERAL_DRIVERS) {
        errno = ENOSPC;
        PS_LOG_ERROR("peripheral", "driver registry is full");
        return -1;
    }

    drivers[driver_count++].driver = driver;
    PS_LOG_DEBUG("peripheral", "registered driver %s", driver->name);
    return 0;
}

void peripheral_bus_stop(void)
{
    unsigned int i;

    for (i = driver_count; i > 0; --i) {
        registered_driver_t *entry = &drivers[i - 1];
        if (!entry->active) continue;
        PS_LOG_TRACE("peripheral", "stopping %s", entry->driver->name);
        if (entry->driver->stop) entry->driver->stop();
        entry->active = false;
    }
    active_classes = 0;
    bus_started = false;
}

int peripheral_bus_start(void)
{
    unsigned int i;

    if (bus_started) {
        PS_LOG_TRACE("peripheral", "bus is already started");
        return 0;
    }

    for (i = 0; i < driver_count; ++i) {
        registered_driver_t *entry = &drivers[i];
        const primeshim_peripheral_driver_t *driver = entry->driver;
        int available;

        if (driver->classes & active_classes) {
            PS_LOG_WARN("peripheral", "skipping %s: class already claimed",
                        driver->name);
            continue;
        }

        available = driver->probe ? driver->probe() : 1;
        if (available <= 0 || driver->start() != 0) {
            PS_LOG_WARN("peripheral", "%s driver %s", driver->name,
                        available <= 0 ? "not available" : "failed to start");
            if (driver->required) {
                peripheral_bus_stop();
                errno = ENODEV;
                return -1;
            }
            continue;
        }

        entry->active = true;
        active_classes |= driver->classes;
        PS_LOG_INFO("peripheral", "started %s (classes 0x%x)", driver->name,
                    driver->classes);
    }

    bus_started = true;
    return 0;
}

uint32_t peripheral_bus_active_classes(void)
{
    return active_classes;
}
