#ifndef PRIMESHIM_STUB_ANW_H
#define PRIMESHIM_STUB_ANW_H
#include <stdint.h>
typedef struct ANativeWindow ANativeWindow;
typedef struct ANativeWindow_Buffer {
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
    void *bits;
    int32_t reserved[6];
} ANativeWindow_Buffer;
enum { WINDOW_FORMAT_RGBA_8888 = 1 };
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width,
                                         int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *buffer,
                           void *dirty);
int32_t ANativeWindow_unlockAndPost(ANativeWindow *window);
#endif
