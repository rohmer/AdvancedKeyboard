# French corpus benchmark results

Measured on the complete prepared training split derived from `D:\Corpus\fr-FR`. Development-pilot measurements are excluded from these tables.

## Corpus and evaluation

- Original TXT input: 2,522,137,862 bytes across three files.
- Prepared training input: 2,339,987,943 bytes.
- Exact training duplicates removed: 1,158,561; held-out sentence overlaps purged: 67,427.
- Content-grouped crawl documents: 15,057 training, 1,946 validation, 1,972 test.
- Final test: 2,500 words, 8,306 incomplete-prefix queries, 1,000 clean correction controls and 1,000 one-scalar-deletion typos.
- Evaluated query panel: 116 held-out documents for completion and 99 for correction.

TXT contains sentence lines with article IDs and boundaries discarded; held-out crawl sentence overlaps are globally purged from Wikipedia training.
Exact normalized document/sentence deduplication; no claim of semantic/paraphrase deduplication.

The same frozen training, word list and sealed test queries are used for every variant. Validation pages were used for development; test results were collected after source freeze. Corpus preparation is a shared preprocessing step and is excluded from generation timing.

## Generation time and memory

Four counting workers; optimized training budget 512 MiB; native generator process-tree private-commit ceiling 2 GiB. Cold means an empty model-training cache with caching enabled. OS file caches were not flushed. Memory below is the native generator Windows job peak private committed bytes, not RSS. These are individual measured runs, not claimed universal speedups.

| Variant | Run | Artifact-ready seconds | Generator peak private MiB | Profiles produced |
|---|---|---:|---:|---|
| Original compact counts | cold | 439.814 | 57.59 | 256 KiB |
| Original compact counts | warm | 91.340 | 34.98 | 256 KiB |
| Original compact counts | profile-reuse | 87.683 | 32.86 | 16 KiB |
| Optimized compact counts | cold | 220.837 | 53.67 | 256 KiB |
| Optimized compact counts | warm | 8.998 | 19.98 | 256 KiB |
| Optimized compact counts | profile-reuse | 8.868 | 17.16 | 16 KiB |
| Radix counts | cold | 238.894 | 86.82 | 256 + 16 KiB batch |
| Radix counts | warm | 23.986 | 82.61 | 256 + 16 KiB batch |
| Radix modified Kneser–Ney | cold | 356.762 | 281.44 | 256 + 16 KiB batch |
| Radix modified Kneser–Ney | warm | 20.249 | 95.97 | 256 + 16 KiB batch |

The preserved native baseline produced the 256 KiB model and index in 439.8 seconds. The previously user-reported 4–5-hour workflow was not reproduced by this measured native CLI workload.

Artifact-ready time includes required index construction. The separate Python index builder used for compact models is outside the measured native generator job: its memory is unmeasured and is not subject to the stated 2 GiB ceiling. Legacy compact profiles require separate invocations; radix exports both sizes from one training pass. Cached small-profile legacy runs reuse the same count/training cache.

## Storage, prediction quality and native lookup latency

Legacy nominal budgets constrain NGTR alone; radix budgets constrain NGTR plus NGIX. Use the actual combined bytes below when comparing quality. Coverage is vocabulary coverage over the same eligible completion queries. All-query accuracy includes out-of-vocabulary targets.

| Variant | Nominal profile | NGTR + NGIX bytes | Top-3 % | Next-word top-3 % | Coverage % | Clean false-change % | Deletion recovery % | Completion p95 µs |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Original compact counts | 256 KiB | 470,297 | 33.939 | 15.000 | 80.689 | 1.500 | 61.900 | 162.00 |
| Original compact counts | 16 KiB | 32,550 | 26.728 | 12.680 | 46.641 | 2.100 | 23.600 | 39.20 |
| Optimized compact counts | 256 KiB | 470,297 | 33.939 | 15.000 | 80.689 | 1.500 | 61.900 | 173.90 |
| Optimized compact counts | 16 KiB | 32,550 | 26.728 | 12.680 | 46.641 | 2.100 | 23.600 | 41.40 |
| Radix counts | 256 KiB | 262,139 | 35.131 | 16.000 | 72.935 | 3.300 | 59.800 | 179.60 |
| Radix counts | 16 KiB | 16,379 | 26.752 | 13.120 | 44.269 | 2.800 | 19.200 | 62.00 |
| Radix modified Kneser–Ney | 256 KiB | 262,132 | 34.517 | 17.040 | 65.916 | 2.100 | 49.200 | 92.70 |
| Radix modified Kneser–Ney | 16 KiB | 16,228 | 25.716 | 13.920 | 40.236 | 1.600 | 14.900 | 26.80 |

## Equal combined-storage budgets

The original compact models set the shared NGTR + NGIX limits. Radix counts and modified Kneser–Ney are generated from the same full-corpus cached training data under those limits. The models may use fewer bytes than the limit. These rows compare retrained models at the same maximum combined storage; their vocabularies can differ.

| Variant | Combined budget bytes | NGTR + NGIX bytes | Top-3 % | Top-3 delta pp | Next-word top-3 % | Coverage % | Clean false-change % | Deletion recovery % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Original compact counts | 470,297 | 470,297 | 33.939 | +0.000 | 15.000 | 80.689 | 1.500 | 61.900 |
| Radix counts | 470,297 | 470,294 | 36.576 | +2.637 | 16.800 | 77.799 | 2.600 | 64.400 |
| Radix modified Kneser–Ney | 470,297 | 470,267 | 36.395 | +2.456 | 18.080 | 72.177 | 2.100 | 56.400 |
| Original compact counts | 32,550 | 32,550 | 26.728 | +0.000 | 12.680 | 46.641 | 2.100 | 23.600 |
| Radix counts | 32,550 | 32,540 | 28.678 | +1.950 | 13.480 | 50.710 | 2.800 | 26.800 |
| Radix modified Kneser–Ney | 32,550 | 32,530 | 27.486 | +0.758 | 14.600 | 44.823 | 2.200 | 21.400 |

## Format-only conversion

The original compact models are converted to radix without retraining. The retained test panel has zero ordered suggestion/correction output fingerprint mismatches and unchanged top-3 accuracy and vocabulary coverage. This measures representation savings and observed test-panel parity; it does not establish behavior for every possible input.

| Original nominal profile | Original NGTR + NGIX bytes | Radix NGTR + NGIX bytes | Storage reduction % | Output fingerprint mismatches | Top-3 delta pp |
|---|---:|---:|---:|---:|---:|
| 256 KiB | 470,297 | 314,193 | 33.193 | 0 | +0.000 |
| 16 KiB | 32,550 | 17,975 | 44.777 | 0 | +0.000 |

## Native runtime memory and latency

Workspace bytes are the desktop ABI subtotal `sizeof(Model) + sizeof(PrefixScratch) + sizeof(AutocorrectScratch)`. They exclude serialized model/index bytes, probe/query overhead and temporary call-stack locals. Lookup latency covers native prediction/correction calls; query parsing and French normalization occur outside those timings. Nominal-profile rows reuse the quality measurements collected after cold generation or cached small-profile export.

| Cohort | Variant | Profile or combined budget | Workspace bytes | Logical nodes | Physical nodes | Completion p50 µs | Completion p95 µs | Correction p50 µs | Correction p95 µs |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| Nominal | Original compact counts | 256 KiB | 11,680 | 95,974 | 95,974 | 24.70 | 162.00 | 1210.30 | 1449.30 |
| Nominal | Original compact counts | 16 KiB | 11,680 | 5,816 | 5,816 | 10.70 | 39.20 | 163.80 | 183.40 |
| Nominal | Optimized compact counts | 256 KiB | 11,736 | 95,974 | 95,974 | 26.90 | 173.90 | 1297.00 | 1525.10 |
| Nominal | Optimized compact counts | 16 KiB | 11,736 | 5,816 | 5,816 | 11.50 | 41.40 | 171.40 | 190.80 |
| Nominal | Radix counts | 256 KiB | 11,736 | 84,985 | 33,342 | 32.30 | 179.60 | 1201.10 | 1398.90 |
| Nominal | Radix counts | 16 KiB | 11,736 | 4,815 | 1,920 | 16.30 | 62.00 | 197.50 | 224.90 |
| Nominal | Radix modified Kneser–Ney | 256 KiB | 11,736 | 69,668 | 23,703 | 21.80 | 92.70 | 1037.90 | 1688.60 |
| Nominal | Radix modified Kneser–Ney | 16 KiB | 11,736 | 3,995 | 1,406 | 9.20 | 26.80 | 151.10 | 222.80 |
| Equal storage | Radix counts | 470,297 bytes | 11,736 | 155,424 | 61,247 | 35.90 | 206.60 | 1616.50 | 1925.90 |
| Equal storage | Radix counts | 32,550 bytes | 11,736 | 9,636 | 3,796 | 18.60 | 74.80 | 340.60 | 384.20 |
| Equal storage | Radix modified Kneser–Ney | 470,297 bytes | 11,736 | 126,440 | 43,155 | 25.60 | 175.60 | 1456.50 | 2456.40 |
| Equal storage | Radix modified Kneser–Ney | 32,550 bytes | 11,736 | 8,191 | 2,822 | 12.50 | 37.90 | 264.00 | 382.40 |
| Format only | Converted radix counts | 256 KiB source | 11,736 | 95,974 | 40,960 | 40.20 | 231.10 | 1888.60 | 2265.60 |
| Format only | Converted radix counts | 16 KiB source | 11,736 | 5,816 | 2,022 | 18.10 | 62.60 | 248.60 | 279.20 |

## Paired document-bootstrap comparisons

Changes are candidate minus baseline in percentage points over the identical test queries. The 95% intervals resample whole content-hashed crawl documents, keeping each document's queries paired. Lower clean false-change rates are better; higher values are better for the other three metrics. An unavailable interval is shown explicitly.

| Comparison | Metric | Queries | Documents | Delta pp | 95% CI pp |
|---|---|---:|---:|---:|---|
| Original to optimized-radix, nominal 256 KiB | Completion top-3 | 8,306 | 116 | +1.192 | [+0.783, +1.586] |
| Original to optimized-radix, nominal 256 KiB | Next-word top-3 | 2,500 | 116 | +1.000 | [+0.525, +1.480] |
| Original to optimized-radix, nominal 256 KiB | Deletion recovery | 1,000 | 99 | -2.100 | [-3.596, -0.642] |
| Original to optimized-radix, nominal 256 KiB | Clean false changes | 1,000 | 99 | +1.800 | [+0.964, +2.683] |
| Original to optimized-smoothed, nominal 256 KiB | Completion top-3 | 8,306 | 116 | +0.578 | [-0.165, +1.269] |
| Original to optimized-smoothed, nominal 256 KiB | Next-word top-3 | 2,500 | 116 | +2.040 | [+0.704, +3.379] |
| Original to optimized-smoothed, nominal 256 KiB | Deletion recovery | 1,000 | 99 | -12.700 | [-15.686, -9.894] |
| Original to optimized-smoothed, nominal 256 KiB | Clean false changes | 1,000 | 99 | +0.600 | [-0.314, +1.623] |
| Radix counts to MKN, 256 KiB combined | Completion top-3 | 8,306 | 116 | -0.614 | [-1.315, +0.060] |
| Radix counts to MKN, 256 KiB combined | Next-word top-3 | 2,500 | 116 | +1.040 | [-0.138, +2.226] |
| Radix counts to MKN, 256 KiB combined | Deletion recovery | 1,000 | 99 | -10.600 | [-12.922, -8.537] |
| Radix counts to MKN, 256 KiB combined | Clean false changes | 1,000 | 99 | -1.200 | [-2.159, -0.112] |
| Original to optimized-radix, equal 470297 B storage | Completion top-3 | 8,306 | 116 | +2.637 | [+2.045, +3.225] |
| Original to optimized-radix, equal 470297 B storage | Next-word top-3 | 2,500 | 116 | +1.800 | [+1.164, +2.491] |
| Original to optimized-radix, equal 470297 B storage | Deletion recovery | 1,000 | 99 | +2.500 | [+1.355, +3.712] |
| Original to optimized-radix, equal 470297 B storage | Clean false changes | 1,000 | 99 | +1.100 | [+0.509, +1.743] |
| Original to optimized-smoothed, equal 470297 B storage | Completion top-3 | 8,306 | 116 | +2.456 | [+1.677, +3.188] |
| Original to optimized-smoothed, equal 470297 B storage | Next-word top-3 | 2,500 | 116 | +3.080 | [+1.761, +4.353] |
| Original to optimized-smoothed, equal 470297 B storage | Deletion recovery | 1,000 | 99 | -5.500 | [-7.649, -3.402] |
| Original to optimized-smoothed, equal 470297 B storage | Clean false changes | 1,000 | 99 | +0.600 | [-0.104, +1.383] |
| Original to optimized-radix, nominal 16 KiB | Completion top-3 | 8,306 | 116 | +0.024 | [-0.288, +0.326] |
| Original to optimized-radix, nominal 16 KiB | Next-word top-3 | 2,500 | 116 | +0.440 | [+0.117, +0.785] |
| Original to optimized-radix, nominal 16 KiB | Deletion recovery | 1,000 | 99 | -4.400 | [-6.034, -2.781] |
| Original to optimized-radix, nominal 16 KiB | Clean false changes | 1,000 | 99 | +0.700 | [+0.179, +1.290] |
| Original to optimized-smoothed, nominal 16 KiB | Completion top-3 | 8,306 | 116 | -1.011 | [-1.794, -0.289] |
| Original to optimized-smoothed, nominal 16 KiB | Next-word top-3 | 2,500 | 116 | +1.240 | [-0.047, +2.486] |
| Original to optimized-smoothed, nominal 16 KiB | Deletion recovery | 1,000 | 99 | -8.700 | [-10.809, -6.505] |
| Original to optimized-smoothed, nominal 16 KiB | Clean false changes | 1,000 | 99 | -0.500 | [-1.379, +0.281] |
| Radix counts to MKN, 16 KiB combined | Completion top-3 | 8,306 | 116 | -1.035 | [-1.737, -0.368] |
| Radix counts to MKN, 16 KiB combined | Next-word top-3 | 2,500 | 116 | +0.800 | [-0.439, +2.020] |
| Radix counts to MKN, 16 KiB combined | Deletion recovery | 1,000 | 99 | -4.300 | [-6.000, -2.743] |
| Radix counts to MKN, 16 KiB combined | Clean false changes | 1,000 | 99 | -1.200 | [-2.178, -0.307] |
| Original to optimized-radix, equal 32550 B storage | Completion top-3 | 8,306 | 116 | +1.950 | [+1.556, +2.363] |
| Original to optimized-radix, equal 32550 B storage | Next-word top-3 | 2,500 | 116 | +0.800 | [+0.390, +1.226] |
| Original to optimized-radix, equal 32550 B storage | Deletion recovery | 1,000 | 99 | +3.200 | [+1.645, +4.835] |
| Original to optimized-radix, equal 32550 B storage | Clean false changes | 1,000 | 99 | +0.700 | [+0.000, +1.449] |
| Original to optimized-smoothed, equal 32550 B storage | Completion top-3 | 8,306 | 116 | +0.758 | [+0.024, +1.511] |
| Original to optimized-smoothed, equal 32550 B storage | Next-word top-3 | 2,500 | 116 | +1.920 | [+0.702, +3.100] |
| Original to optimized-smoothed, equal 32550 B storage | Deletion recovery | 1,000 | 99 | -2.200 | [-3.868, -0.452] |
| Original to optimized-smoothed, equal 32550 B storage | Clean false changes | 1,000 | 99 | +0.100 | [-0.829, +0.975] |

The full JSON retains top-1, MRR, in-vocabulary metrics and calibrated log loss where available. Log loss is not compared across different vocabulary/OOV mappings; legacy priority fallback has no calibrated probability model. Desktop timings do not establish ESP32 hardware latency.

## Reproducibility

- Corpus manifest SHA-256: `77c699da5c79b8602276f5f0d6556da8c89f5596e7d1d6de6976f48bcc3659e6`.
- Test query SHA-256: `4601cdcf3ea5dc630d393f4be47e28ae45a374a4d8eda214183f2fd90bded5b6`.
- Final generator executable SHA-256: `26f4c5e79b26b48317059a8efca1df4322b9601b4d7d9456410fa31603c9c9a3`.
- Final prediction probe SHA-256: `7971fcb433b932c68f6bf285fc5686ab9393b6cd31a0fe2d9c17b6d14d14e4ed`.
- Split seed: `lvgl-fr-FR-2026-11-24-v1`; query protocol: `v3: content-document groups; incomplete prefixes0/1/2/3; runtime French normalization; dot/newline context resets; one token NLL per prefix0`.

The optimized compact measurements use the preserved optimized compact snapshot. Subsequent source-freeze guard fixes did not affect its counting path. The radix measurements use the final generator and prediction probe recorded in the source manifest.

| Variant | Generator SHA-256 | Prediction probe SHA-256 |
|---|---|---|
| Original compact counts | `11332901c4133b1f8995c5ec0e3e64b919019ad3e0adff9d320c4ea7353e30d0` | `c20f4fe9bf579fcae8b7034eb6b987141f96c333c7ce1c1015183bf7fc8a0caf` |
| Optimized compact counts | `0a4427d844fd938ca0763003f491738f63604263912963f58919cbc44fcb39eb` | `7ad1b626036bdd03fa084ea4e4a68be13e4b822ff4edf6abb46c783fbbd01cec` |
| Radix counts | `26f4c5e79b26b48317059a8efca1df4322b9601b4d7d9456410fa31603c9c9a3` | `7971fcb433b932c68f6bf285fc5686ab9393b6cd31a0fe2d9c17b6d14d14e4ed` |
| Radix modified Kneser–Ney | `26f4c5e79b26b48317059a8efca1df4322b9601b4d7d9456410fa31603c9c9a3` | `7971fcb433b932c68f6bf285fc5686ab9393b6cd31a0fe2d9c17b6d14d14e4ed` |

Host and build environment: CPU: Intel Core i9-12900K (16 cores / 24 logical processors); OS: Windows 11 Pro 10.0.26300 (build 26300); RAM: 68,494,786,560 bytes physical RAM; Compiler: MSVC toolset 14.51.36231, Hostx64/x64; CMake: 4.5.1; Python: 3.11.9; Build mode: Release, x64, Ninja.

Retained artifacts: corpus/input hashes, derived training hashes, source manifests, every command line, generation phase timestamps, model/index hashes, native per-query outcome fingerprints and document-clustered paired comparisons. See the adjacent README for rerunning the harness.
