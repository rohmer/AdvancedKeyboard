# Native language-model generator

Build from this repository root with CMake and a C++17 compiler:

```sh
cmake -S tools/NativeTrieGenerator -B build/native-trie -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-trie --config Release --parallel
tools/NativeTrieGenerator/bin/UICreatorModelGen --language fr --format radix --smoothing counts --memory-mib 512 --trie-size 262144 --profile-sizes 16384,262144 --trie-output model.ngtr -- corpus-folder
```

Use `.exe` on Windows. Source files and wordlists are included under `source`;
the native tool compiles against this repository's `tools/ModelCodec` and `src`
reader. It has no Creator, ESPOS, LVGL, .NET or board-driver dependency. Wordlists
are copied beside the executable. `ADVKBD_NATIVE_HELPER_OUTPUT` overrides the
default ignored helper output directory. Set `UICREATOR_MODELGEN` to a helper
stored elsewhere so the Windows Corpus Workbench can discover it.

Fresh GUI settings use radix v4 + Counts. Existing CLI scripts keep legacy
v2 + Counts when format/scoring flags are omitted; `--compact` writes v3.
Radix `--trie-size` includes NGTR plus NGIX and needs at least 64 bytes;
compatibility caps cover NGTR only (v3 minimum 20, v2 minimum 40). Keep a radix
model and its matching index together when deploying. Omit the size for an
unlimited export, subject to the configured training workspace.

`--smoothing modified-kn` enables radix-only interpolated word-trigram training
and normalized pruned backoff. It is opt-in because metadata/training costs can
reduce coverage under small budgets; evaluate the language/profile you use.
`--memory-mib` defaults to 512 and bounds shared algorithm workspaces rather
than imposing an OS process-memory ceiling. Worker count can be reduced to fit.
`--profile-sizes N,N` reuses counting/training and writes sibling size profiles.

`--no-cache` disables count/training cache reuse. `--no-training-cache` rebuilds
combined/ranked/scored artifacts while retaining per-file count reuse.
`--cache-dir` and `--scratch-directory` choose cache/temp locations; prefer an
unsynced local working disk. `--json-output` exports selected counts/scores;
`--capabilities` reports supported features as JSON. Each final file uses atomic
replacement; a whole batch/model-index pair is not one filesystem transaction.

The supported languages and normalization/vocabulary policies are documented
in `source/README.md`. Statistical details are in `source/TrainingModel.md`.
Retained provenance and vocabulary/Unicode licenses are in `source/PROVENANCE.md`
and `source/wordlists`. Downloaded corpora and caches are not bundled.
