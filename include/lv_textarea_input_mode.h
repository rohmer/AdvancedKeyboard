#ifndef LV_TEXTAREA_INPUT_MODE_H
#define LV_TEXTAREA_INPUT_MODE_H
#include <lvgl.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Alpha Numeric is intentionally unrestricted, including Unicode/punctuation.
 * Numeric accepts ASCII 0-9 and at most one decimal point; no sign/exponent.
 * Case conversion uses Unicode mappings and preserves accents and scripts. */
typedef enum {
    LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC = 0,
    LV_ADVANCED_KEYBOARD_INPUT_UPPERCASE = 1,
    LV_ADVANCED_KEYBOARD_INPUT_LOWERCASE = 2,
    LV_ADVANCED_KEYBOARD_INPUT_NUMERIC = 3
} lv_advanced_keyboard_input_mode_t;

/* Works independently of a keyboard. The textarea owns its mode until deletion.
 * Existing accepted_chars/max_length filters still apply. Normalizes existing
 * content; typing, pastes and programmatic changes use the same restriction.
 * VALUE_CHANGED is emitted when the mode/content changes. LVGL thread only.
 * Returns false for invalid objects/modes or allocation failure. */
bool lv_advanced_keyboard_set_textarea_input_mode(lv_obj_t* textarea,
    lv_advanced_keyboard_input_mode_t mode);
lv_advanced_keyboard_input_mode_t lv_advanced_keyboard_get_textarea_input_mode(const lv_obj_t* textarea);

/* Returns required UTF-8 bytes excluding NUL. A buffer of required+1 bytes
 * receives the entire result. Short buffers receive a complete-codepoint prefix
 * and NUL when capacity>0. Input/output buffers must not overlap. Numeric
 * normalization keeps the first decimal point. Invalid UTF-8 is omitted in
 * restricted modes; unrestricted input is copied verbatim. */
size_t lv_advanced_keyboard_normalize_input(lv_advanced_keyboard_input_mode_t mode,
    const char* input, char* output, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
