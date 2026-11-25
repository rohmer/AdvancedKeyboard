#include "include/lv_textarea_input_mode.h"
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t first, last; int32_t delta; uint8_t step; } case_range_t;
typedef struct { uint32_t scalar, values[3]; uint8_t count; } case_expansion_t;
#include "UnicodeCase.inc"
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#if LVGL_VERSION_MAJOR < 9
#define mode_malloc lv_mem_alloc
#define mode_free lv_mem_free
#define mode_send(obj, code) lv_event_send((obj), (code), NULL)
#else
#define mode_malloc lv_malloc
#define mode_free lv_free
#define mode_send(obj, code) lv_obj_send_event((obj), (code), NULL)
#endif

static size_t decode(const char* input, uint32_t* scalar)
{
    const unsigned char* bytes = (const unsigned char*)input;
    uint32_t value = bytes[0];
    size_t width, i;
    if(value < 0x80) { *scalar = value; return 1; }
    if(value < 0xC2 || value > 0xF4) return 0;
    width = value < 0xE0 ? 2 : value < 0xF0 ? 3 : 4;
    value &= 0x7F >> width;
    for(i = 1; i < width; ++i) {
        if(!bytes[i] || (bytes[i] & 0xC0) != 0x80) return 0;
        if(i == 1 && ((bytes[0] == 0xE0 && bytes[i] < 0xA0) ||
            (bytes[0] == 0xED && bytes[i] >= 0xA0) ||
            (bytes[0] == 0xF0 && bytes[i] < 0x90) ||
            (bytes[0] == 0xF4 && bytes[i] > 0x8F))) return 0;
        value = (value << 6) | (bytes[i] & 0x3F);
    }
    *scalar = value;
    return width;
}
static void append(uint32_t scalar, char* output, size_t capacity, size_t* used, bool* full)
{
    const size_t width = scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
    size_t i;
    if(output && !*full && *used < capacity && width < capacity - *used) {
        if(width == 1) output[*used] = (char)scalar;
        else {
            uint32_t rest = scalar;
            for(i = width - 1; i; --i) { output[*used + i] = (char)(0x80 | (rest & 0x3F)); rest >>= 6; }
            output[*used] = (char)((0xFFu << (8 - width)) | rest);
        }
        output[*used + width] = '\0';
    }
    else *full = true;
    *used += width;
}
static uint32_t map_range(uint32_t scalar, const case_range_t* ranges, size_t count)
{
    size_t begin = 0, end = count;
    while(begin < end) {
        size_t middle = begin + (end - begin) / 2;
        if(ranges[middle].last < scalar) begin = middle + 1;
        else end = middle;
    }
    if(begin < count && scalar >= ranges[begin].first &&
        (scalar - ranges[begin].first) % ranges[begin].step == 0)
        return (uint32_t)((int32_t)scalar + ranges[begin].delta);
    return scalar;
}
static const case_expansion_t* expansion(uint32_t scalar, const case_expansion_t* entries, size_t count)
{
    size_t begin = 0, end = count;
    while(begin < end) {
        size_t middle = begin + (end - begin) / 2;
        if(entries[middle].scalar < scalar) begin = middle + 1;
        else end = middle;
    }
    return begin < count && entries[begin].scalar == scalar ? &entries[begin] : NULL;
}
static size_t normalize(lv_advanced_keyboard_input_mode_t mode, const char* input,
    char* output, size_t capacity, bool decimal)
{
    size_t used = 0;
    bool full = false;
    if(output && capacity) output[0] = '\0';
    if(!input) return 0;
    if(mode == LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC) {
        used = strlen(input);
        if(output && capacity) {
            size_t copy = used < capacity ? used : capacity - 1;
            if(copy < used) while(copy && ((unsigned char)input[copy] & 0xC0) == 0x80) --copy;
            memcpy(output, input, copy); output[copy] = '\0';
        }
        return used;
    }
    while(*input) {
        uint32_t scalar;
        size_t width = decode(input, &scalar), i;
        const case_expansion_t* expanded = NULL;
        if(!width) { ++input; continue; }
        input += width;
        if(mode == LV_ADVANCED_KEYBOARD_INPUT_NUMERIC) {
            if(scalar == '.' && !decimal) decimal = true;
            else if(scalar < '0' || scalar > '9') continue;
        }
        else if(mode == LV_ADVANCED_KEYBOARD_INPUT_UPPERCASE) {
            expanded = expansion(scalar, kUpperExpansions, COUNT(kUpperExpansions));
            if(!expanded) scalar = map_range(scalar, kUpperRanges, COUNT(kUpperRanges));
        }
        else if(mode == LV_ADVANCED_KEYBOARD_INPUT_LOWERCASE) {
            expanded = expansion(scalar, kLowerExpansions, COUNT(kLowerExpansions));
            if(!expanded) scalar = map_range(scalar, kLowerRanges, COUNT(kLowerRanges));
        }
        if(expanded) for(i = 0; i < expanded->count; ++i) append(expanded->values[i], output, capacity, &used, &full);
        else append(scalar, output, capacity, &used, &full);
    }
    return used;
}
size_t lv_advanced_keyboard_normalize_input(lv_advanced_keyboard_input_mode_t mode,
    const char* input, char* output, size_t capacity)
{
    return normalize(mode, input, output, capacity, false);
}

#if LV_USE_TEXTAREA
typedef struct input_mode_state {
    struct input_mode_state* next;
    lv_obj_t* field;
    char* replacement;
    size_t capacity;
    unsigned references;
    lv_advanced_keyboard_input_mode_t mode;
    bool busy, deleted;
} input_mode_state;
static input_mode_state* fields;
static input_mode_state* find(const lv_obj_t* field)
{
    input_mode_state* state;
    for(state = fields; state; state = state->next) if(state->field == field) return state;
    return NULL;
}
static void release(input_mode_state* state)
{
    if(--state->references) return;
    mode_free(state->replacement);
    mode_free(state);
}
static bool reserve(input_mode_state* state, size_t size)
{
    char* bytes;
    if(size <= state->capacity) return true;
    bytes = (char*)mode_malloc(size);
    if(!bytes) return false;
    mode_free(state->replacement);
    state->replacement = bytes; state->capacity = size;
    return true;
}
static size_t normalized_cursor(lv_advanced_keyboard_input_mode_t mode, const char* text, size_t cursor)
{
    size_t count = 0, index = 0;
    bool decimal = false;
    while(*text && index++ < cursor) {
        uint32_t scalar;
        const size_t width = decode(text, &scalar);
        const case_expansion_t* mapped = NULL;
        if(!width) { ++text; continue; }
        text += width;
        if(mode == LV_ADVANCED_KEYBOARD_INPUT_NUMERIC) {
            if(scalar == '.' && !decimal) decimal = true;
            else if(scalar < '0' || scalar > '9') continue;
        }
        else if(mode == LV_ADVANCED_KEYBOARD_INPUT_UPPERCASE) mapped = expansion(scalar, kUpperExpansions, COUNT(kUpperExpansions));
        else if(mode == LV_ADVANCED_KEYBOARD_INPUT_LOWERCASE) mapped = expansion(scalar, kLowerExpansions, COUNT(kLowerExpansions));
        count += mapped ? mapped->count : 1;
    }
    return count;
}
static bool already_normalized(lv_advanced_keyboard_input_mode_t mode, const char* text, bool decimal)
{
    if(mode == LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC) return true;
    while(*text) {
        uint32_t scalar;
        const size_t width = decode(text, &scalar);
        if(!width) return false;
        text += width;
        if(mode == LV_ADVANCED_KEYBOARD_INPUT_NUMERIC) {
            if(scalar == '.' && !decimal) decimal = true;
            else if(scalar < '0' || scalar > '9') return false;
        }
        else if(mode == LV_ADVANCED_KEYBOARD_INPUT_UPPERCASE) {
            if(expansion(scalar, kUpperExpansions, COUNT(kUpperExpansions)) ||
                map_range(scalar, kUpperRanges, COUNT(kUpperRanges)) != scalar) return false;
        }
        else if(mode == LV_ADVANCED_KEYBOARD_INPUT_LOWERCASE) {
            if(expansion(scalar, kLowerExpansions, COUNT(kLowerExpansions)) ||
                map_range(scalar, kLowerRanges, COUNT(kLowerRanges)) != scalar) return false;
        }
    }
    return true;
}
/* Returns true when an actual content change was made. The state is retained
 * by the caller because VALUE_CHANGED handlers may delete the field. */
static int normalize_field(input_mode_state* state)
{
    const char* text = lv_textarea_get_text(state->field);
    size_t required;
    size_t mapped_cursor;
    /* Normal typing is already normalized by INSERT. Avoid whole-field scratch
     * allocation and avoid resizing a replacement LVGL is still consuming. */
    if(already_normalized(state->mode, text, false)) return false;
    required = normalize(state->mode, text, NULL, 0, false);
    if(!reserve(state, required + 1)) return -1;
    normalize(state->mode, text, state->replacement, state->capacity, false);
    if(!strcmp(text, state->replacement)) return false;
    /* Normalization can expand letters (sharp S) or remove numeric characters. */
    mapped_cursor = normalized_cursor(state->mode, text, lv_textarea_get_cursor_pos(state->field));
    state->busy = true;
    lv_textarea_set_text(state->field, state->replacement);
    if(!state->deleted) {
        state->busy = false;
        lv_textarea_set_cursor_pos(state->field, (int32_t)mapped_cursor);
    }
    return true;
}
static void on_field(lv_event_t* event)
{
    input_mode_state* state = (input_mode_state*)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    ++state->references;
    if(code == LV_EVENT_DELETE) {
        input_mode_state** link = &fields;
        while(*link && *link != state) link = &(*link)->next;
        if(*link) *link = state->next;
        state->deleted = true;
        release(state);
    }
    else if(!state->busy && state->mode != LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC) {
        if(code == LV_EVENT_INSERT) {
            const char* input = (const char*)lv_event_get_param(event);
            /* LVGL sends a delete marker through INSERT before backspace. */
            if(input && *input && input != state->replacement && !(input[0] == LV_KEY_DEL && !input[1])) {
                bool decimal = state->mode == LV_ADVANCED_KEYBOARD_INPUT_NUMERIC &&
                    strchr(lv_textarea_get_text(state->field), '.') != NULL;
                /* With accepted_chars/max_length LVGL consumes a replacement
                 * one character at a time, recursively emitting INSERT. Those
                 * already-normalized characters must not overwrite the source
                 * buffer (e.g. truncate the outer sharp-S expansion "SS"). */
                if(!already_normalized(state->mode, input, decimal)) {
                    size_t size = normalize(state->mode, input, NULL, 0, decimal);
                    if(!reserve(state, size + 1)) lv_textarea_set_insert_replace(state->field, "");
                    else {
                        normalize(state->mode, input, state->replacement, state->capacity, decimal);
                        if(strcmp(input, state->replacement)) lv_textarea_set_insert_replace(state->field, state->replacement);
                    }
                }
            }
        }
        else if(code == LV_EVENT_VALUE_CHANGED && normalize_field(state) > 0) {
            /* The nested normalized event already notified consumers. */
            lv_event_stop_processing(event);
        }
    }
    release(state);
}
bool lv_advanced_keyboard_set_textarea_input_mode(lv_obj_t* field, lv_advanced_keyboard_input_mode_t mode)
{
    input_mode_state* state;
    bool changed;
    int normalized;
    lv_advanced_keyboard_input_mode_t previous;
    if(!field || mode < LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC || mode > LV_ADVANCED_KEYBOARD_INPUT_NUMERIC) return false;
#if LVGL_VERSION_MAJOR < 9
    if(!lv_obj_check_type(field, &lv_textarea_class)) return false;
#else
    if(!lv_obj_has_class(field, &lv_textarea_class)) return false;
#endif
    state = find(field);
    if(!state) {
        if(mode == LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC) return true;
        state = (input_mode_state*)mode_malloc(sizeof(*state));
        if(!state) return false;
        memset(state, 0, sizeof(*state)); state->field = field; state->references = 1;
        /* LVGL 8 stores the filter in an 8-bit enum bit-field. MSVC sign
         * extends its 0x80 PREPROCESS bit, so the dispatcher never matches it.
         * Ordinary INSERT callbacks still run before accepted_chars/length
         * checks; use those on this toolchain and retain preprocessing elsewhere. */
#if LVGL_VERSION_MAJOR < 9 && defined(_MSC_VER)
        if(!lv_obj_add_event_cb(field, on_field, LV_EVENT_ALL, state)) {
#else
        if(!lv_obj_add_event_cb(field, on_field, (lv_event_code_t)(LV_EVENT_ALL | LV_EVENT_PREPROCESS), state)) {
#endif
            mode_free(state); return false;
        }
        state->next = fields; fields = state;
    }
    ++state->references;
    previous = state->mode;
    changed = state->mode != mode;
    state->mode = mode;
    normalized = mode == LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC ? 0 : normalize_field(state);
    if(normalized < 0) { state->mode = previous; release(state); return false; }
    if(changed && !normalized && !state->deleted) mode_send(field, LV_EVENT_VALUE_CHANGED);
    changed = !state->deleted;
    release(state);
    return changed;
}
lv_advanced_keyboard_input_mode_t lv_advanced_keyboard_get_textarea_input_mode(const lv_obj_t* field)
{
    const input_mode_state* state = find(field);
    return state ? state->mode : LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC;
}
#else
bool lv_advanced_keyboard_set_textarea_input_mode(lv_obj_t* field, lv_advanced_keyboard_input_mode_t mode)
{ (void)field; (void)mode; return false; }
lv_advanced_keyboard_input_mode_t lv_advanced_keyboard_get_textarea_input_mode(const lv_obj_t* field)
{ (void)field; return LV_ADVANCED_KEYBOARD_INPUT_ALPHANUMERIC; }
#endif
