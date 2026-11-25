#pragma once
#include <stdbool.h>
#include <stdint.h>

// Test-only declarations for compiling the public C prediction facade. This is
// not an LVGL implementation and is never linked into the widget library.
typedef struct lv_obj_t lv_obj_t;
typedef struct { uint8_t blue, green, red; } lv_color_t;
