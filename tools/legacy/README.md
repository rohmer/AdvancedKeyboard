# Legacy Python trainer

`ngram_model.py` preserves the original ESPOS Python training algorithm and CLI
byte for byte. It uses Python's Unicode case folding, per-period/line boundaries,
in-memory frequency counters and its original cache format. It writes NGTR v2;
it does not produce compressed radix v4 or Kneser–Ney scores.

```sh
python tools/legacy/ngram_model.py corpus.txt --no-cache --trie-output model-v2.ngtr
python tools/legacy/ngram_model.py --help
```

For current models, bounded memory and large corpora, use the native generator
under `tools/NativeTrieGenerator` with explicit `--format radix --smoothing counts`
or `--smoothing modified-kn`. The portable native reader remains compatible with
the legacy output. Existing ESPOS Python entry points delegate here; do not copy
this implementation back into a consumer. Keep corpora, caches and outputs outside
maintained source. This import retains its existing ownership and notices; no
separate ESPOS-wide source license was supplied.
