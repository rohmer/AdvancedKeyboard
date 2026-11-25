# Model build utilities

`generate_bundled_models.py` emits selected catalog registries for native builds.

`pack_ngram_partition.py` creates an ESPOS-compatible NGPK1 image containing an
NGTR v2/v3/v4 model, its canonical NGIX index and the SHA-256 of the model:

```sh
python tools/models/pack_ngram_partition.py model.ngtr ngram.bin 0x20000
```

The three positional arguments retain the old ESPOS command interface. The
original 64 KiB input-model cap remains; the model, index and 48-byte header
must also fit the requested partition size. The output is replaced only after
validation and a complete temporary write. The input and output must differ.
For index-only output, use `python tools/layout/build_ngtr_index.py model.ngtr model.ngix`.
