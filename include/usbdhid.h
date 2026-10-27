#ifndef USBDHID_H
#define USBDHID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Struct holding the required callbacks
typedef struct {
    // 1. Callback when the device is plugged in and configured by the host
    void (*on_ready)(void);

    // 2. Callback when the device is unplugged or suspended
    void (*on_plug_out)(void);

    // 4. Callback when an OUT packet arrives from the host
    void (*on_out_packet)(const void* buffer, int len);
} usbdhid_callbacks_t;

// Attach HID to the bound ConfigFS gadget and start the background thread.
int usbdhid_init(const usbdhid_callbacks_t* callbacks);

// 3. Function to check whether the gadget is currently configured and ready for communication
bool usbdhid_is_ready(void);

// 4.1. Function to toggle whether the gadget is actively receiving packets from the host
void usbdhid_set_rx_enabled(bool enabled);

// 5. Function to send a packet to the host.
// It automatically splits 'len' into 64-byte chunks and retries if busy up to 'timeout_ms'.
int usbdhid_send(const void* data, size_t len, int timeout_ms);

// Cleanly stop the background thread without dismantling the composite gadget.
void usbdhid_cleanup(void);

int usbd_deviceio_handler(int request,void* buffer,int len);

#ifdef __cplusplus
}
#endif

#endif // USBDHID_H
