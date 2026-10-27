#ifndef PRIMESHIM_INPUT_H
#define PRIMESHIM_INPUT_H

#include <stdbool.h>

void enqueue_key_packet(int device_keycode, int is_press);
void init_touch_system(void);
void update_touch_slot(int slot_id, int x, int y, bool is_down);
void commit_touch_frame(void);

#endif
