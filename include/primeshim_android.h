#ifndef PRIMESHIM_ANDROID_H
#define PRIMESHIM_ANDROID_H

/* Bridge between the Android Java UI (via JNI) and the firmware runtime.
 *
 * All functions are safe to call from any thread. Input entry points may be
 * called before the firmware has started; events are dropped with a warning
 * until the peripheral bus is running.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;

/* Display -------------------------------------------------------------- */

/* Attach (or detach, with NULL) the Android surface the runtime should draw
 * into.  Takes a reference on the window; the previous one is released. */
void primeshim_android_set_window(struct ANativeWindow *window);

/* Copy the live 320x240 firmware framebuffer (0x00RRGGBB per pixel) into
 * `destination`, which must hold at least 320*240 uint32_t values. */
void primeshim_android_snapshot(uint32_t *destination);

/* Invalidate the cached frame diff so the next composition always posts. */
void primeshim_android_request_redraw(void);

/* Input ---------------------------------------------------------------- */

enum {
    PRIMESHIM_TOUCH_DOWN = 1,
    PRIMESHIM_TOUCH_MOVE = 2,
    PRIMESHIM_TOUCH_UP = 3
};

/* Coordinates are already in firmware space (0..319, 0..239). */
void primeshim_android_touch(int action, int x, int y);

/* Multi-touch: update one firmware slot without committing, then commit all
 * slots once so the firmware receives one TOUCH batch per Android event. */
void primeshim_android_touch_update(int action, int slot, int x, int y);
void primeshim_android_touch_commit(void);

/* key is an HP Prime keycode (see map_android_key() for the translation). */
void primeshim_android_key(int key, int pressed);

#ifdef __cplusplus
}
#endif

#endif
