#ifndef PRIMESHIM_PERIPHERAL_H
#define PRIMESHIM_PERIPHERAL_H

#include <stdbool.h>
#include <stdint.h>

enum primeshim_peripheral_class {
    PRIMESHIM_PERIPHERAL_DISPLAY  = 1u << 0,
    PRIMESHIM_PERIPHERAL_KEYBOARD = 1u << 1,
    PRIMESHIM_PERIPHERAL_POINTER  = 1u << 2,
    PRIMESHIM_PERIPHERAL_USB      = 1u << 3
};

typedef struct primeshim_peripheral_driver {
    const char *name;
    uint32_t classes;
    bool required;
    int (*probe)(void);
    int (*start)(void);
    void (*stop)(void);
} primeshim_peripheral_driver_t;

int peripheral_driver_register(const primeshim_peripheral_driver_t *driver);
int peripheral_bus_start(void);
void peripheral_bus_stop(void);
uint32_t peripheral_bus_active_classes(void);

/* Static drivers register before main(), similarly to built-in kernel drivers. */
#define PRIMESHIM_REGISTER_PERIPHERAL_DRIVER(driver_symbol)                 \
    static void __attribute__((constructor))                                \
    primeshim_register_##driver_symbol(void)                                \
    {                                                                       \
        (void)peripheral_driver_register(&(driver_symbol));                 \
    }

#endif
