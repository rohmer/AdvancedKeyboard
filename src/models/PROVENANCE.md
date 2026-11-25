# Built-in English model

Imported at the user's request from their ESPOS `Trie/enUS16.ngtr`.

The current built-in is a lossless NGTR v4 radix/Patricia re-encoding with an
NGIX v2 index. It retains all 5,691 logical nodes and exact terminal counts;
its 2,096 physical edges require 14,888 model bytes and 3,476 index bytes
(18,364 combined, down from 32,011). Predictions and correction decisions are
unchanged. `builtin.json` records machine-readable current/source metadata.
`PredictionBuiltin.inc` is regenerated from these two binary files.

- Current model SHA-256: `1b056647ea6c66460545ca0a3ccbd68ece85a44680c611e17ad10f73d4a0b75a`.
- Current index SHA-256: `dd4b5a9c40ec0f1b7b4f8299d293ef21b2025e21f835457c2cb043ec1ea56d32`.

Original imported assets (retained as regression/provenance references):

- NGTR v3 model: 16384 bytes; SHA-256 `ddff8e652ca7177e79e8e661e68a62b9edc2ddcaaea62f1af993bf423926b6fc`.
- NGIX v1 lookup index: 15627 bytes; SHA-256 `9759ccc1ffa5bcf35bacc9df5d44fd1edc72595afb2f6900c41de5613ba6785e`. Generated with the unchanged ESPOS `tools/build_ngtr_index.py`, retained as `../layout/build_ngtr_index.py`.

The source checkout does not provide a model-specific license or corpus manifest for this prebuilt trie. Its existing ownership and applicable terms remain unchanged. The source generator's explicit English word-list notice is retained in `EnglishWordList.LICENSE.md`; that notice covers the word list, rather than asserting a license for an unspecified training corpus. No Spanish, French, German or Greek word lists are included.
