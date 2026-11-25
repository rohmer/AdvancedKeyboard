# Build and integrate Advanced Keyboard

Use CMake 3.24 or later, a C++17/C99 compiler and an existing LVGL **9.6** checkout
or target. CMake 4.5.1 is supported. CMake uses only explicitly supplied sources;
it does not fetch LVGL, a toolchain, SDK or corpus. Use separate build directories
for different architectures, compilers and LVGL configurations.

## Windows and Linux native component

From a Visual Studio x64/x86 developer shell on Windows, or a normal GCC/Clang
shell on Linux:

```sh
cmake -S . -B build/component -G Ninja -DCMAKE_BUILD_TYPE=Release -DADVKBD_LVGL_SOURCE_DIR=/path/to/lvgl -DLV_BUILD_CONF_PATH=/path/to/lv_conf.h
cmake --build build/component --parallel
```

Quote paths containing spaces. For the Visual Studio generator, use separate
trees with `-A x64` or `-A Win32`, build using `--config Release`, and omit `-G
Ninja`. An application normally adds this directory after its existing LVGL
target and links `AdvancedKeyboard::AdvancedKeyboard`.

The LVGL headers/configuration must match the linked LVGL library, including
color depth, allocator and enabled features. `ADVKBD_LVGL_TARGET` selects a custom
existing target; otherwise `lvgl::lvgl` or `lvgl` is detected. The separate library
mode supports an imported static LVGL archive. Embedding requires a buildable
LVGL target, and a parent project with both C and C++ enabled:

```cmake
project(MyFirmware LANGUAGES C CXX)
add_subdirectory(lvgl)
set(ADVKBD_EMBED_IN_LVGL ON CACHE BOOL "")
add_subdirectory(AdvancedKeyboard)
target_link_libraries(firmware PRIVATE AdvancedKeyboard::AdvancedKeyboard)
```

Only the keyboard's own sources are attached. The library does not enable or
compile LVGL's other optional widgets. Required LVGL settings are
`LV_USE_BUTTON=1`, `LV_USE_LABEL=1`, `LV_USE_TEXTAREA=1` and UTF-8
text support. The stock `LV_USE_KEYBOARD` may remain zero. Display/touch drivers,
`lv_init()`, ticks and `lv_timer_handler()` belong to the application.

In embedded mode the engine object files enter the LVGL archive too. The same
compiled objects can back the separate headless target for tools/tests; they are
not compiled a second time or linked twice into the firmware.

## Features and minimum firmware

| CMake option | Default | Behavior |
| --- | --- | --- |
| `ADVKBD_BUILD_WIDGET` | `ON` | Build the LVGL widget/C facade; requires LVGL. |
| `ADVKBD_BUILD_PREDICTION` | `ON` | Native prediction/autocorrect engine; also usable without LVGL. |
| `ADVKBD_BUILTIN_MODEL` | `ON` | Small English model in the widget predictor; ignored with prediction off. |
| `ADVKBD_ENABLE_PINYIN` | `OFF` | Mandarin composition dictionary and candidate paging. |
| `ADVKBD_SCRIPT_FONTS` | `OFF` | Enable all five additional keyboard fonts. |
| `ADVKBD_FONT_HINDI`, `BENGALI`, `CYRILLIC`, `GREEK`, `URDU` | `OFF` | Individual font switches, each using the `ADVKBD_FONT_` prefix. |
| `ADVKBD_BUNDLED_MODELS` | `none` | Semicolon-separated `language:KiB` catalog profiles, or `all`. |
| `ADVKBD_EMBED_IN_LVGL` | `OFF` | Attach widget sources directly to an existing buildable LVGL target. |
| `ADVKBD_BUILD_TOOLS`, `ADVKBD_BUILD_TESTS`, `ADVKBD_BUILD_EXAMPLE` | `OFF` | Explicit host tools, regressions and UI example. |

For a keyboard with an application-supplied suggestion callback and no native
trie engine, set `ADVKBD_BUILD_PREDICTION=OFF`. This excludes Model/Prefix/Storage
and the C predictor implementation from the source list. UTF-8 normalization
remains available to the widget. Disable `ADVKBD_BUILTIN_MODEL` when loading only
custom models. No offline encoder, trainer, crawler or desktop UI is compiled
into the on-device component.

The prediction target is `AdvancedKeyboard::Prediction`; it exposes native
`PredictionModel.h`, `PredictionPrefix.h` and `PredictionAutocorrect.h` from `src`.
It includes no LVGL widget or C predictor facade. `Prediction.cpp` belongs to the
widget library. Allocate one scratch workspace per concurrent prediction caller;
model bytes must outlive every borrowing model/predictor. Cursors are invalidated
by reopening/closing their model. Normal inference performs no C++ allocations.

## Scripts, RTL and target text

All layout definitions are available independently of font options. Additional
fonts cover Hindi, Bengali, Russian/Cyrillic, Greek and Urdu key labels. Select
only the needed `ADVKBD_FONT_*` options, or use the master switch to enable all.
Field fonts remain application-owned and must cover the typed script.

Mandarin Pinyin needs `ADVKBD_ENABLE_PINYIN=ON` plus a font covering Chinese
candidate characters, such as LVGL's `LV_FONT_SOURCE_HAN_SANS_SC_16_CJK` option.
Arabic can use `LV_FONT_DEJAVU_16_PERSIAN_HEBREW` or a supplied font. Configure
those LVGL fonts when compiling LVGL, rather than defining the flags only on
the keyboard target. `LV_USE_BIDI=1` enables RTL ordering. Keep
`LV_USE_ARABIC_PERSIAN_CHARS=0` for native textarea input; LVGL's contextual
label rewriting can change the editable text. Stock LVGL does not provide full
Arabic/Urdu contextual shaping or Hindi/Bengali OpenType positioning in these
editable fields. The Windows simulator uses the operating system's renderer.

## Selected bundled models

The catalog is `models/bundled/catalog.json`. To add only French 16 KiB and
Mandarin 256 KiB selections:

```sh
cmake -S . -B build/component -DADVKBD_LVGL_SOURCE_DIR=/path/to/lvgl -DLV_BUILD_CONF_PATH=/path/to/lv_conf.h "-DADVKBD_BUNDLED_MODELS=fr-FR:16;zh-CN:256" -DADVKBD_ENABLE_PINYIN=ON
```

Python 3.10+ generates immutable C++ arrays and NGIX indexes in the build tree.
The registry requires both widget and prediction options. Its public feature
macro is `ADVKBD_HAS_BUNDLED_MODELS=1`; APIs in `espos_keyboard/BundledModels.h`
are linked only when the registry is enabled. Call `Count`, `At`, `Find`, `Apply`,
`Selected` or `Clear` on the LVGL thread. Applying a profile changes the key
layout and owns a predictor workspace for that keyboard. The original English
default is independent; disable `ADVKBD_BUILTIN_MODEL` if it is unnecessary.
Catalog labels such as 16/256 KiB describe NGTR profiles: include their generated
NGIX bytes, fonts, application and scratch in the board's storage/RAM budget.

## ESP-IDF

Add this directory as a project component using `EXTRA_COMPONENT_DIRS` or place
it under `components/advanced_keyboard`. Provide an existing LVGL component
named `lvgl`; the supplied `idf_component.yml` does not download LVGL. Select
options before the project's `project()` call:

```cmake
set(EXTRA_COMPONENT_DIRS /path/to/AdvancedKeyboard /path/to/lvgl)
set(ADVKBD_BUILTIN_MODEL OFF CACHE BOOL "")
set(ADVKBD_BUNDLED_MODELS "fr-FR:16" CACHE STRING "")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_firmware)
```

Use ESP-IDF 5.2 or later and set `CONFIG_COMPILER_CXX_EXCEPTIONS=y` in the
application's `sdkconfig.defaults` (or enable C++ exceptions in `menuconfig`).
The component uses IDF's C++17, exception, heap and LVGL integration. It compiles
the native engine once; board/display initialization and ESPOS partition/startup
adapters are separate. On ESP32, immutable model/index copies and inference
scratch prefer PSRAM where available and retain the original-storage/internal
fallback. Enable/configure PSRAM for the actual board in its own `sdkconfig`.
Headless IDF prediction uses `ADVKBD_BUILD_WIDGET=OFF` and needs no LVGL component.

## Arduino and PlatformIO

Install this directory as an Arduino library alongside LVGL 9.6. Public forwarding
headers under `src` keep Arduino's single include-root convention compatible with
the canonical `include` tree. The board must provide a C++17-capable standard
library and C++ exceptions; not every board advertised by Arduino has the memory
or compiler support for LVGL/prediction. ESP32 and suitably provisioned RP2040/
RP2350 ports are the intended embedded families.
The metadata uses the dependency-version range and archive mode defined by the
[Arduino library specification](https://docs.arduino.cc/arduino-cli/library-specification/).

Set the corresponding macros in the board's `lv_conf.h` (before its use), or in
consistent global compiler flags:

```c
#define LVSERIAL_USE_ADVANCED_KEYBOARD_PREDICTION 1
#define LVSERIAL_USE_ADVANCED_KEYBOARD_BUILTIN_TRIE 0
#define LV_ADVANCED_KEYBOARD_USE_PINYIN 0
#define LV_ADVANCED_KEYBOARD_USE_HINDI_FONT 0
#define LV_ADVANCED_KEYBOARD_USE_BENGALI_FONT 0
#define LV_ADVANCED_KEYBOARD_USE_CYRILLIC_FONT 0
#define LV_ADVANCED_KEYBOARD_USE_GREEK_FONT 0
#define LV_ADVANCED_KEYBOARD_USE_URDU_FONT 0
```

Arduino compiles library sources into an archive; disabled/unreferenced objects
are omitted by its normal linker. The CMake source list can avoid compiling
disabled engine files entirely. PlatformIO can supply `-std=gnu++17` and
`-fexceptions` via its build flags when its board defaults differ.

For an Arduino bundled registry, run the supplied generator explicitly with the
desired selection and output `src/bundled_models.generated.cpp`, then rebuild
the library. Include `espos_keyboard/BundledModels.h` in the sketch when using
the registry. Put initialization after the board's LVGL display/input setup;
the component never creates a hardware display or changes rotation.

## Example and host-only tools

`ADVKBD_BUILD_EXAMPLE=ON` builds `advanced_keyboard_example`, a small C scene
with an ordinary field and numeric field bound to one keyboard. Link it into
an application and call `advanced_keyboard_example_create(parent)` after LVGL
display initialization. It does not install a simulator/display backend.

Native tools/tests can build without LVGL:

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DADVKBD_BUILD_WIDGET=OFF -DADVKBD_BUILD_TOOLS=ON -DADVKBD_BUILD_TESTS=ON
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

Use `--config Release` and `-C Release` with a multi-configuration generator.
The managed Corpus Workbench and keyboard simulator require the .NET 8 SDK and
Windows desktop support; build them explicitly using their tool guides. Corpus
downloads/crawls, training data and personal settings are not part of the source
snapshot or default CMake component build.
