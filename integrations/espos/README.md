# Optional ESPOS adapters

These compatibility sources are kept outside the portable component. The root
build does not compile them or any ESPOS board/display/startup drivers.

- `include/ngram_trie` forwards the ESPOS model/prefix/autocorrect names to the
  portable engine. Add this `include` directory and the component's `src` to
  the consuming target's include paths, then link `AdvancedKeyboard::Prediction`.
- `esp_runtime.cpp` supplies ESP-IDF partition loading, shared model PSRAM and
  per-predictor scratch for the optional `ngram` partition. Build it against the
  engine and IDF's `esp_partition`, `heap` and `log` components. The partition
  image uses the existing ESPOS `NGPK` v1 header; arbitrary NGTR bytes alone
  are not a partition image. Applications can instead pass NGTR/NGIX spans
  directly to the portable predictor without this adapter.
- `TypingView.cpp` and `include/espos_keyboard/LegacyTypingView.h` provide the
  ESPOS demo view's language/size/orientation controls. Build them against the
  widget and a selected bundled registry (`ADVKBD_BUNDLED_MODELS`), with LVGL's
  dropdown and checkbox features enabled. Define
  `ESPOS_KEYBOARD_LEGACY_VIEW_ADAPTER=1` consistently for callers of this API.
  The board provides `create_default_typing_display()` when that helper is used.

New applications can use `lv_advanced_keyboard_*` directly, bind externally
owned textareas, and supply their own display, input and model lifetime policy.
No dependency on the ESPOS checkout is required by this source tree.
