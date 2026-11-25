#pragma once
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Call on the LVGL thread after creating the application's display/input port. */
void advanced_keyboard_example_create(lv_obj_t* parent);
#ifdef __cplusplus
}
#endif
