# Advanced Keyboard

Portable C/C++ keyboard for **LVGL 9.6**, with an external textarea, multilingual
layouts, input constraints, prediction and autocorrect. Applications supply the
LVGL display, tick and input-device ports. Board drivers are outside this library.

Repository: [rohmer/AdvancedKeyboard](https://github.com/rohmer/AdvancedKeyboard).

This repository includes the keyboard runtime, public headers,
layout/model authoring tools, native model generator, Windows Corpus Workbench
and simulator sources, tests, model catalog and notices. CMake builds the on-device
component by default. Offline tools and tests are explicit opt-ins.

## Add the component to an existing LVGL project

```cmake
# Your project creates lvgl or lvgl::lvgl first, using its own lv_conf.h.
add_subdirectory(path/to/AdvancedKeyboard)
target_link_libraries(my_app PRIVATE AdvancedKeyboard::AdvancedKeyboard)
```

Or set `ADVKBD_EMBED_IN_LVGL=ON` before `add_subdirectory` to compile the keyboard
widget sources directly into the existing LVGL target. `ADD_TO_LVGL=ON` is an
initial-configuration alias. An imported LVGL archive uses the default separate
static-library mode. No LVGL dependency is downloaded or vendored automatically.

Enable `LV_USE_BUTTON`, `LV_USE_LABEL` and `LV_USE_TEXTAREA` in the
application's LVGL configuration. The stock LVGL keyboard widget is not required.
The library uses C++17, C99 and C++ exceptions for recoverable allocation failures.

```c
#include <lv_advanced_keyboard.h>

lv_obj_t *name = lv_textarea_create(parent);
lv_textarea_set_one_line(name, true);
lv_obj_t *amount = lv_textarea_create(parent);
lv_advanced_keyboard_set_textarea_input_mode(amount,
    LV_ADVANCED_KEYBOARD_INPUT_NUMERIC);

lv_obj_t *keyboard = lv_advanced_keyboard_create(parent);
lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
lv_advanced_keyboard_bind_textarea(keyboard, name);
lv_advanced_keyboard_bind_textarea(keyboard, amount);
```

Focus or click either bound field to open the keyboard. It follows the selected
field, keeps it visible above the keys, and remains open until Enter/Done or X.
Typing changes the target textarea. Upper/lowercase and numeric modes follow
each field automatically; the keyboard has no private text box.

## Languages and model selection

Layouts include English US, Spanish, French AZERTY, German QWERTZ, Italian,
Portuguese, Greek, Mandarin Pinyin, Arabic, Hindi, Bengali, Indonesian, Urdu and
Russian. `lv_advanced_keyboard_set_language()` selects a layout; a prediction
model is a separate choice. The default built-in model is a small English model.

For additional script fonts and Mandarin composition in CMake:

```cmake
set(ADVKBD_SCRIPT_FONTS ON CACHE BOOL "")
set(ADVKBD_ENABLE_PINYIN ON CACHE BOOL "")
```

Arabic/CJK glyph coverage is configured in LVGL or provided by the application;
the additional bundled font switch does not rebuild LVGL fonts. Arabic and Urdu
start RTL; enable `LV_USE_BIDI=1` and keep `LV_USE_ARABIC_PERSIAN_CHARS=0` for
canonical editable textarea content. See the build guide for glyph/shaping limits.

NGTR v2/v3 and compressed v4 models remain readable. V4 supports exact 64-bit
counts and optional Kneser–Ney probabilities. The decoder borrows immutable
bytes and allocates no C++ heap storage during inference. ESP32 prefers shared
PSRAM copies when available, falling back to the original model storage.

To compile selected catalog models and enable native language/size controls:

```cmake
set(ADVKBD_BUNDLED_MODELS "en-US:16;fr-FR:16" CACHE STRING "")
```

Then use `espos_keyboard/BundledModels.h` and
`espos::keyboard::bundled::Apply(keyboard, "fr-FR", 16)`. Only selected model/index
arrays enter firmware. `none` is the default; `all` is explicit and may exceed
the board's flash capacity. Python runs at build time for this optional registry.

## Source layout

| Directory | Purpose |
| --- | --- |
| `src`, `include` | On-device runtime, public APIs and generated layout/font data |
| `src/models` | Built-in English NGTR v4/NGIX v2 model and provenance |
| `models/bundled` | Optional language/size catalog and immutable model files |
| `tools/ModelCodec` | Offline radix encoder, excluded from runtime sources |
| `tools/layout`, `tools/models`, `tools/fonts` | Authoring and model-packaging tools |
| `tools/NativeTrieGenerator` | C++ corpus-to-model trainer |
| `tools/CorpusWorkbench`, `tools/KeyboardSimulator` | Optional Windows managed tools |
| `tests`, `examples` | Native regressions and a board-independent UI example |
| `integrations/espos` | Optional ESPOS compatibility/partition adapters |

See [component build instructions](BUILD.md), the original runtime notes under
`src/README.md`, and the exported tool guides. License/provenance notices stay
with the source, fonts, Unicode tables and models. See [NOTICE.md](NOTICE.md)
for the preserved Creator, ESPOS and third-party ownership and license terms.
