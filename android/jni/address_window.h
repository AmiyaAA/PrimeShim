#ifndef PRIMESHIM_ADDRESS_WINDOW_H
#define PRIMESHIM_ADDRESS_WINDOW_H

#include <stddef.h>
#include <stdint.h>

/* The firmware is a non-PIE ET_EXEC that must live at 0x30600000.  On Android
 * that region competes with ART's own reservations, so we claim it as early as
 * possible (JNI_OnLoad) with PROT_NONE and hand it back right before the loader
 * maps the real image. */
#define PRIMESHIM_FW_WINDOW_BASE 0x30600000u
#define PRIMESHIM_FW_WINDOW_SIZE (16u * 1024u * 1024u)

int primeshim_address_window_reserve(uint32_t base, size_t size);
int primeshim_address_window_release(void);

#endif
