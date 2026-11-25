# Portable corpus-to-model engine

These native sources provide `UICreatorLanguageModels` and `UICreatorModelGen`
independently of any editor or board project. Build and command-line examples
are in the parent `README.md`. The CMake files use this repository's
`tools/ModelCodec`, `src` reader and adjacent `../Utils/ExecutablePaths.h`;
there are no fallback paths into a Creator or ESPOS checkout.

Supported generator languages are English, Spanish, French, German, Greek,
Arabic, Hindi, Mandarin Chinese, Bengali, Indonesian, Urdu and Russian. The
first five ship vocabulary filters; Spanish/French/German/Greek retain their
bundled-list requirement. Other languages support a custom UTF-8 vocabulary.
Arabic and Indic profiles retain combining marks/joiners; Mandarin uses Han
character n-grams and groups Latin word fragments. This is not dictionary-based
Chinese word segmentation.

Fresh Corpus Workbench settings choose radix v4 + Counts; existing explicit
settings and compatibility formats remain available. Radix size profiles cap
the combined model+index bytes, preserve exact 64-bit counts and reuse global
counting/training caches. The shared RAM workspace defaults to 512 MiB; this
is not an OS process-memory limit. Modified Kneser-Ney is opt-in and trades
metadata/training costs against vocabulary/context coverage at the chosen size.
See `TrainingModel.md` for its statistical/pruning semantics and limitations.

Bundled vocabulary/Unicode licensing is retained under `wordlists`. Original
source lineage and Unicode table generation are recorded in `PROVENANCE.md`.
