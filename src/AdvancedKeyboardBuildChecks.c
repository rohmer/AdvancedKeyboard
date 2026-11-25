#include <lvgl.h>

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR < 6
#error "AdvancedKeyboard requires the LVGL 9.6 API (LVGL 9.6 or later 9.x)."
#endif
#if !LV_USE_BUTTON || !LV_USE_LABEL || !LV_USE_TEXTAREA
#error "AdvancedKeyboard needs LV_USE_BUTTON, LV_USE_LABEL and LV_USE_TEXTAREA enabled in lv_conf.h."
#endif

/* The component adds its own widget implementation. It does not need LVGL's
 * built-in keyboard, tabview, menu, list, chart or other optional widgets. */
