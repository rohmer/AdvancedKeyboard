# Bundled corpus tries

These sixteen NGTR v4 radix/Patricia files were rebuilt losslessly from the
user-supplied NGTR v3 files in `D:/Corpus`.
They cover English (US), Spanish, French, German, Greek, Arabic, Hindi and Mandarin Chinese, each
with nominal 16 KiB and 256 KiB variants. No text corpora or Wikipedia dumps are
included. No additional upstream license or training-data provenance is claimed.

`catalog.json` records current model/index versions, SHA-256 hashes, byte
lengths, logical node counts and compressed physical edge counts. Original
source hashes, sizes, versions and logical node counts remain alongside them.
Vocabulary, terminal counts, ordering and prediction/correction behavior are
preserved; conversion adds no smoothing or trained probabilities.
Source aliases are retained there:
`esMS` is the model from the `es-MX` corpus and uses the shared Spanish `es-ES`
keyboard; `arAR`, `hiHI` and `elEL` map to `ar-SA`, `hi-IN` and `el-GR` layouts.
`zhZH` maps to the `zh-CN` Mandarin Pinyin keyboard. The Mandarin models use
individual Han characters and supply next-character unigram/bigram predictions;
their current data contains no trigram entries.
The 16/256 KiB names identify the original vocabulary profiles. They are not new
limits on the combined radix model and index: enforcing those limits would
require pruning/retraining. Model selection preserves these profile names;
project payloads and footprint reporting use the actual `bytes + indexBytes`.
NGIX v2 indexes are generated deterministically from the model and validated
against `indexBytes` and `indexSHA256`. Han character data may have a larger
NGTR payload after conversion while using less combined storage.

Consumers can stage these files under `language-models/bundled` beside their
executables. LVGL UI Creator reads the small `catalog.json`; model bytes are
loaded and validated when selected. Development builds can read this directory
from their pinned AdvancedKeyboard checkout. There is no runtime dependency on
`D:/Corpus`.

Choosing a bundled model imports its exact bytes through the normal project trie
catalog, generates and retains an NGIX index, and deduplicates equal models.
Multiple widgets share one embedded model. Device/project generation retains only
referenced models, so the full bundled library is not copied into firmware.

From the standalone AdvancedKeyboard root, rebuild the existing catalog and
separate built-in English model using the portable tool:

```sh
cmake -S tools -B build/native-tools -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-tools --parallel
python tools/RadixModel/rebuild_bundled_models.py --catalog models/bundled/catalog.json --converter build/native-tools/RadixModel/UICreatorRadixConvert --index-builder tools/layout/build_ngtr_index.py --builtin-dir src/models --builtin-include src/PredictionBuiltin.inc --work-dir build/rebuild-models --write
```

On Windows add `.exe` to the converter path. Omitting `--write` prepares and
validates every model and prints a plan. Publication backs up changed files
under the supplied work directory and rolls back a failed write. Repeating the
same rebuild is a no-op. Existing v4 scored models are retained with a freshly
validated canonical index. Conversion cannot recover unrecorded corpus counts
or train probabilities from them.

To import the original sixteen source profiles, run the canonical importer from
this repository's root after building the converter above:

```sh
python tools/models/import_corpus.py /path/to/corpus --converter build/native-tools/RadixModel/UICreatorRadixConvert
```

The importer validates and converts all sixteen sources before publishing any
model, updates `models/bundled/catalog.json`, and backs up changed outputs under
`build/keyboard-model-import`. It never changes the input files. No compiled
Creator catalog is generated. Commit model/catalog changes here, then update
consumer dependency pins. Existing saved projects retain their embedded
legacy bytes and IDs; choosing a rebuilt catalog profile imports its new
content-derived identity. The reserved `builtin-en-us` ID stays unchanged.
