# Advanced Keyboard host tools

These tools are self-contained in this repository. Native tools build on
Windows, Linux and macOS with CMake 3.24+ and a C++17 compiler. The managed
Corpus Workbench, crawler, Wikipedia extractor, simulator and test frontends
require Windows and the .NET 8 desktop SDK/runtime. The managed NgramTrie CLI
and NgramTrie.Reader library target portable .NET 8.

```sh
cmake -S tools -B build/native-tools -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-tools --config Release --parallel
```

The native generator is `tools/NativeTrieGenerator/bin/UICreatorModelGen`
(`.exe` on Windows), with its wordlists beside it. That ignored output path
allows the managed UI to find it automatically. Override
`ADVKBD_NATIVE_HELPER_OUTPUT` to choose another directory and set
`UICREATOR_MODELGEN` to that executable when using the UI. The offline
`AdvancedKeyboard::ModelCodec` library and radix converter are host tools;
they are not included in the firmware library.

```powershell
dotnet build tools/CorpusWorkbench/CorpusWorkbench.csproj -c Release
build/corpus-tools/bin/CorpusWorkbench/Release/net8.0-windows/CorpusWorkbench.exe
dotnet build tools/KeyboardSimulator.Tests/KeyboardSimulator.LanguageTests.csproj -c Release
dotnet build tools/CorpusLanguageTests/CorpusLanguageTests.csproj -c Release
dotnet build/corpus-tools/bin/KeyboardSimulator.LanguageTests/Release/net8.0-windows/KeyboardSimulator.LanguageTests.dll tools/layout --native tools/NativeTrieGenerator/bin/UICreatorModelGen.exe
dotnet build/corpus-tools/bin/CorpusLanguageTests/Release/net8.0-windows/CorpusLanguageTests.dll . tools/NativeTrieGenerator/bin/UICreatorModelGen.exe
```

All managed outputs and intermediates are under `build/corpus-tools`. Project
references and managed source files retain their upstream layout. Keyboard JSON
comes exclusively from `tools/layout` and is copied to each app's `Keyboards`
directory. Legacy language-model test fixtures are in
`tests/fixtures/language-models`; tests write generated data under `build`.
`--policy-probe <native radix_prediction executable>` additionally compares
native/managed correction decisions and confidence on matching safety fixtures.

Fresh GUI settings select radix v4 + Counts. The radix budget includes model
and index together; old v2/v3 exports retain their NGTR-only budget. Modified
Kneser-Ney is opt-in. RAM budgets, batches and these format/scoring controls
require the native helper and never silently fall back to incompatible managed
options. See `NativeTrieGenerator/README.md` and its `source/TrainingModel.md`.

Pinned NuGet dependencies are restored during build. Seven collection language
profiles use the Unicode boundary fallback without Java. The six original
OpenNLP collection profiles need JDK 17+ and downloaded models: run
`tools/CorpusCrawler/setup-opennlp.ps1` before building when those profiles are
needed. Downloaded JARs, Java paths, model caches and corpora are deliberately
not bundled. The Java worker source/setup script are included.

Preserved notices include `NgramTrie/Wordlist/LICENSE.md`, native
`source/wordlists/LICENSE.md` and `Unicode-LICENSE.txt`,
`KeyboardSimulator/PinyinData.LICENSE.txt`, generator `source/PROVENANCE.md`,
and dependency licenses supplied by NuGet/OpenNLP. The English spelling/fuzzer
workflow retains its original English and QWERTY assumptions.

## Portable compatibility tools

`KeyboardParity` exports managed correction/prefix/regression references and
replays them against the native headless engine. Its C# project references the
canonical `GBUTest`; its CMake project links the canonical prediction sources.
Build directly with `cmake -S tools/KeyboardParity -B build/keyboard-parity`, or
enable `ADVKBD_BUILD_KEYBOARD_PARITY=ON` in the tools build. See its README for
the unchanged exporter arguments, native replay and input hash verification.

`legacy/ngram_model.py` is the original ESPOS Python trainer, preserved byte for
byte for existing CLI/import users. It retains NGTR v2 output and its original
cache behavior; prefer the native generator for current radix models and large
corpora. `models/pack_ngram_partition.py model.ngtr image.bin 0x20000` packs a
validated NGTR v2/v3/v4 model and canonical index into an ESPOS-compatible NGPK1
partition image. ESPOS compatibility entrypoints resolve these tools through
the pinned dependency rather than maintaining copies.
