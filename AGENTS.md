# Advanced Keyboard

This repository owns the shared implementation. Edit it here; Creator and ESPOS
consume pinned revisions. Consumer UI, board drivers and deployment settings stay
in their respective repositories. Do not restore source-copy or export sync flows.

- `src` and `include`: portable C99/C++17 LVGL runtime and public APIs.
- `tools/ModelCodec`: offline radix encoder; keep it out of firmware source lists.
- `tools/NativeTrieGenerator/source`: native trainer, Unicode tables and word lists.
- `tools/layout`, `tools/fonts`, `tools/models`, `tools/RadixModel`: authoring tools.
- `models/bundled` and `src/models`: catalog assets, built-in model and provenance.
- `tests`: native, codec, tool and host-fake ESP32/Arduino allocation regressions.

Preserve model/font/Unicode notices and unknown user data. Regenerate maintained
assets from their authoring inputs, retain source hashes and validate canonical
NGTR/NGIX bytes. Keep corpora, downloaded datasets and build products untracked.
`PROVENANCE/initial-creator-import.json` is an immutable historical import record.

For portable native/tool/model changes, build from a compiler developer shell:

```sh
cmake -S . -B build/host-tests -DCMAKE_BUILD_TYPE=Release -DADVKBD_BUILD_WIDGET=OFF -DADVKBD_BUILD_TOOLS=ON -DADVKBD_BUILD_TESTS=ON
cmake --build build/host-tests --config Release --parallel
ctest --test-dir build/host-tests -C Release --output-on-failure
```

Runtime widget changes also need a real LVGL 9.6 consumer build; see `BUILD.md`.
Host allocator tests do not execute firmware on a board. Update consumer pins only
after publishing the validated upstream commit. See `NOTICE.md` for source terms.
