# Lossless radix conversion

This host tool re-encodes an unscored NGTR v2/v3 model as compressed v4 with an
NGIX v2 index, without retraining or changing terminal counts. Build from this
standalone repository with CMake and a C++17 compiler:

```sh
cmake -S tools/RadixModel -B build/radix-convert -DCMAKE_BUILD_TYPE=Release
cmake --build build/radix-convert --config Release
build/radix-convert/UICreatorRadixConvert input.ngtr input.ngix output.ngtr output.ngix
```

On Windows append `.exe`; Visual Studio generators place it under the selected
configuration directory. Output paths must be different new files. Pass `-`
instead of the input index for fixed-record v2; v3 needs its matching index.
`tools/layout/build_ngtr_index.py` generates canonical indexes if needed.
Input files are limited to 16 MiB and validated before conversion. Independently
authored combined/unigram roots that disagree cannot be represented losslessly
by this shared-vocabulary encoder and are rejected. The runtime still reads
those legacy models. Compression alone does not improve prediction quality.

To rebuild the whole bundled catalog and separate built-in English model, use
`rebuild_bundled_models.py` with the explicit paths in
`models/bundled/README.md`. It preserves vocabulary/counts and source metadata,
regenerates indexes/C++ arrays, stages all models before publication, backs up
changed files and preserves concurrent edits. Omit `--write` for a plan;
repeating the same rebuild changes no files.

```sh
python -m unittest discover -s tools/RadixModel -p test_rebuild_bundled_models.py -v
```
