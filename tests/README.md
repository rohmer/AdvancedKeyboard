# Standalone tests

No Creator or ESPOS checkout is needed. The native suites use this repository's
`src`, `include`, model codec and native trainer. The tiny files in
`fixtures/language-models` are authored reference corpora and C#-generated golden
outputs; `reference.json` preserves their source/reference assembly hashes.
No external corpus, personal settings or generated build output is included.
`native/radix/LegacyV3.inc` preserves the former built-in English arrays so
legacy decoding/conversion remains covered after the production model migrates
to radix. The original model provenance/notices remain under `src/models`.

From the repository root, use a C/C++17 compiler, CMake 3.24+ and Python 3.10+:

```sh
cmake -S . -B build/host-tests -DCMAKE_BUILD_TYPE=Release -DADVKBD_BUILD_WIDGET=OFF -DADVKBD_BUILD_PREDICTION=ON -DADVKBD_BUILD_TOOLS=ON -DADVKBD_BUILD_TESTS=ON
cmake --build build/host-tests --config Release --parallel
ctest --test-dir build/host-tests -C Release --output-on-failure
```

On Windows run these commands from a Visual Studio C++ developer prompt. On
Linux use GCC or Clang. A direct `cmake -S tests -B build/tests` build is also
supported. Tests use host-only LVGL type declarations to compile the real C
prediction facade; they do not link a fake widget or pretend to verify rendering.

The suites cover legacy and radix decoding, codec/index byte parity, cached
node/count/score metadata, Unicode prediction, autocorrect decisions, per-query
C++ allocations, a real C consumer, modified Kneser–Ney normalization/external
merges, generator goldens/cache/publication/cancellation, and ESP32/Arduino PSRAM
placement/sharing/fallback through a fake host allocator. Host allocator tests
are not device execution or latency tests.
The shared bundled-model Python suite checks every exported language/profile's
NGTR v4 headers, logical/physical counts, source provenance, content identity,
SHA-256 and canonical NGIX v2 hash, plus selected-only deterministic C++ arrays.
It also compares the built-in English binary assets, metadata and embedded C++
arrays, and rejects forged format, node-count or index-hash declarations.

`ADVKBD_TEST_GENERATOR=OFF` omits trainer/generator checks.
`ADVKBD_TEST_PSRAM=OFF` omits fake-allocator platform variants.
Managed simulator and corpus/workbench checks live under `tools`; their project
references and fixtures remain inside this repository. See `tools/README.md`.
Benchmark helpers and historical reports are under `tools/benchmarks`; a full
French corpus benchmark is explicit opt-in and is not part of CTest.
