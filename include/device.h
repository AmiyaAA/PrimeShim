#ifndef PRIMESHIM_DEVICE_H
#define PRIMESHIM_DEVICE_H

#include <stdint.h>

typedef int32_t (*primeshim_device_open_fn)(void **open_context);
typedef void (*primeshim_device_close_fn)(void *open_context);
typedef int32_t (*primeshim_device_ioctl_fn)(
    void *open_context, uint32_t request, const void *input,
    uint32_t input_size, void *output, uint32_t output_size);

typedef struct primeshim_device_driver {
    /* The final component of the guest path, for example "USB" for
     * "\\\\?\\USB". Matching is ASCII case-insensitive. The descriptor and
     * its name must remain valid for the life of the process. */
    const char *name;
    primeshim_device_open_fn open;
    primeshim_device_close_fn close;
    primeshim_device_ioctl_fn io_control;
} primeshim_device_driver_t;

int device_driver_register(const primeshim_device_driver_t *driver);
uint32_t device_open(const char *guest_path);
int32_t device_io_control(uint32_t handle, uint32_t request,
                          const void *input, uint32_t input_size,
                          void *output, uint32_t output_size,
                          uint32_t *result);
int device_close(uint32_t handle);

/* Static drivers register before main(), similarly to built-in kernel drivers. */
#define PRIMESHIM_REGISTER_DEVICE_DRIVER(driver_symbol)                     \
    static void __attribute__((constructor))                                \
    primeshim_register_device_##driver_symbol(void)                         \
    {                                                                       \
        (void)device_driver_register(&(driver_symbol));                     \
    }

#endif
