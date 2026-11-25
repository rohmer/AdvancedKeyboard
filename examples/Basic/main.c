#include "advanced_keyboard_example.h"
#include "lv_advanced_keyboard.h"

void advanced_keyboard_example_create(lv_obj_t* parent)
{
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(page, 12, 0);

    lv_obj_t* name = lv_textarea_create(page);
    lv_obj_set_size(name, LV_PCT(100), 44);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
    lv_textarea_set_one_line(name, true);
    lv_textarea_set_placeholder_text(name, "Tap to enter your name");

    lv_obj_t* amount = lv_textarea_create(page);
    lv_obj_set_size(amount, LV_PCT(100), 44);
    lv_obj_align_to(amount, name, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
    lv_textarea_set_one_line(amount, true);
    lv_textarea_set_placeholder_text(amount, "Tap to enter an amount");
    lv_advanced_keyboard_set_textarea_input_mode(amount, LV_ADVANCED_KEYBOARD_INPUT_NUMERIC);

    lv_obj_t* keyboard = lv_advanced_keyboard_create(parent);
    if(!keyboard) return;
    lv_obj_set_size(keyboard, LV_PCT(100), 200);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_advanced_keyboard_set_language(keyboard, "en-US");
    lv_advanced_keyboard_bind_textarea(keyboard, name);
    lv_advanced_keyboard_bind_textarea(keyboard, amount);
    /* Focus/click either external field to open the keyboard. Enter or X
     * dismisses it. The active field is kept visible above the keyboard. */
}
