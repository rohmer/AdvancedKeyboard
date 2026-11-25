# French radix lookup and correction optimization

The same full-corpus model/index bytes are compared before and after the runtime changes. Radix lookups are faster in this Windows experiment; the stricter correction policy suppresses wrong changes and also loses some useful corrections. Results below keep those effects separate.

## Corpus, frozen panels and limits

- Original text input: 2,522,137,862 bytes; prepared training: 2,339,987,943 bytes.
- All 6 original TXT/crawl inputs were rehashed after lookup measurements: identical inventory, sizes and SHA-256 hashes; no corpus writes.
- Validation was the only panel used to select policy thresholds. The previous published test is explicitly an exposed regression panel.
- Fresh confirmation excludes all 116 document headers in that exposed test, leaving 1,834 unused held-out documents with retained sentences.
- Frozen confirmation: 2,500 words, 8,326 incomplete-prefix queries, 1,000 clean controls and 1,000 one-scalar-deletion typos; 119 completion documents and 109 correction documents.
- Confirmation was opened only after source/policy freeze and accepted validation results. No subsequent threshold tuning used it.

All held-out sentences had already been purged from the prepared training files. The Wikipedia TXT lost article boundaries; this is a crawl-document holdout, not a Wikipedia article split. Exact normalized deduplication does not establish semantic independence. The new panel comes from the same crawl, and synthetic deletion errors do not cover natural user typing errors.

## Repeated native lookup measurements

Eight native processes per runtime/model, pinned to logical CPU 2; runtime order rotates and reverses. Every process preloads/normalizes the same queries, warms one complete pass and measures the next pass. Other builds and heavy I/O were held idle. Entries are medians of the eight per-run p50/p95 measurements, in microseconds; per-run values and min/max ranges are retained in JSON. These are desktop prediction calls, not end-to-end LVGL or ESP32 hardware latency.

| Artifact | Nominal profile | Prefix scalars | Baseline p50 / p95 µs | Speed-only p50 / p95 µs | Final p50 / p95 µs | Final p95 change % |
|---|---:|---:|---:|---:|---:|---:|
| Radix counts | 256 KiB | 0 | 129.05 / 189.45 | 44.00 / 65.95 | 43.75 / 65.30 | -65.53 |
| Radix counts | 256 KiB | 1 | 36.30 / 111.65 | 13.40 / 41.35 | 13.35 / 40.95 | -63.32 |
| Radix counts | 256 KiB | 2 | 25.35 / 82.00 | 9.70 / 28.80 | 9.70 / 28.30 | -65.49 |
| Radix counts | 256 KiB | 3 | 15.30 / 44.70 | 6.50 / 14.70 | 6.50 / 14.60 | -67.34 |
| Radix counts | 16 KiB | 0 | 53.60 / 63.50 | 17.60 / 24.30 | 17.50 / 24.10 | -62.05 |
| Radix counts | 16 KiB | 1 | 19.70 / 28.10 | 6.60 / 11.25 | 6.60 / 11.25 | -59.96 |
| Radix counts | 16 KiB | 2 | 10.70 / 19.25 | 4.00 / 6.80 | 4.00 / 6.80 | -64.68 |
| Radix counts | 16 KiB | 3 | 6.20 / 12.20 | 2.90 / 5.00 | 2.95 / 5.05 | -58.61 |
| Radix MKN | 256 KiB | 0 | 70.70 / 192.30 | 34.60 / 113.15 | 34.45 / 113.05 | -41.21 |
| Radix MKN | 256 KiB | 1 | 23.50 / 49.00 | 13.30 / 26.25 | 13.25 / 26.55 | -45.82 |
| Radix MKN | 256 KiB | 2 | 17.40 / 41.05 | 9.30 / 23.10 | 9.30 / 23.65 | -42.39 |
| Radix MKN | 256 KiB | 3 | 13.20 / 30.35 | 7.00 / 15.45 | 7.00 / 15.85 | -47.78 |
| Radix MKN | 16 KiB | 0 | 22.05 / 31.00 | 12.05 / 16.85 | 12.10 / 16.95 | -45.32 |
| Radix MKN | 16 KiB | 1 | 9.90 / 20.60 | 5.40 / 11.50 | 5.40 / 11.50 | -44.17 |
| Radix MKN | 16 KiB | 2 | 6.70 / 15.05 | 3.50 / 8.05 | 3.50 / 8.10 | -46.18 |
| Radix MKN | 16 KiB | 3 | 5.25 / 10.60 | 2.70 / 5.40 | 2.70 / 5.45 | -48.58 |
| Original compact v3 | 256 KiB | 0 | 140.10 / 173.10 | 146.75 / 182.15 | 146.30 / 180.05 | +4.02 |
| Original compact v3 | 256 KiB | 1 | 24.60 / 105.05 | 25.50 / 110.85 | 25.65 / 110.95 | +5.62 |
| Original compact v3 | 256 KiB | 2 | 22.50 / 91.50 | 23.35 / 95.05 | 23.40 / 94.95 | +3.77 |
| Original compact v3 | 256 KiB | 3 | 11.30 / 42.80 | 11.80 / 44.70 | 11.80 / 44.60 | +4.21 |
| Original compact v3 | 16 KiB | 0 | 37.80 / 42.20 | 39.30 / 43.70 | 39.00 / 43.35 | +2.73 |
| Original compact v3 | 16 KiB | 1 | 13.60 / 23.40 | 14.20 / 24.50 | 14.15 / 24.50 | +4.70 |
| Original compact v3 | 16 KiB | 2 | 7.30 / 15.45 | 7.70 / 16.30 | 7.70 / 16.15 | +4.53 |
| Original compact v3 | 16 KiB | 3 | 2.90 / 10.65 | 3.10 / 11.05 | 3.00 / 11.10 | +4.23 |

Speed-only preserves every ordered suggested/corrected word on these panels. Fingerprints cover ordered words, not suggestion masses or correction confidence; numeric/cache metadata is checked separately by native fixtures. Final preserves every ordered completion word while applying the fixed correction gates. The v3 rows show a small measured slowdown and are retained as compatibility evidence; acceleration is not claimed for every format. Different artifact rows retain different vocabularies and storage costs, so compare runtimes within a row.

| Artifact | Profile | Input kind | Baseline p50 / p95 µs | Speed-only p50 / p95 µs | Final p50 / p95 µs |
|---|---:|---|---:|---:|---:|
| Radix counts | 256 KiB | clean | 1250.95 / 1626.55 | 778.25 / 938.15 | 6.65 / 851.90 |
| Radix counts | 256 KiB | delete | 1222.10 / 1581.85 | 755.45 / 922.00 | 742.95 / 922.40 |
| Radix counts | 16 KiB | clean | 200.10 / 216.80 | 114.50 / 126.95 | 112.95 / 126.35 |
| Radix counts | 16 KiB | delete | 194.55 / 213.30 | 108.85 / 122.50 | 110.00 / 125.05 |
| Radix MKN | 256 KiB | clean | 1075.70 / 1716.10 | 638.20 / 937.85 | 14.10 / 729.25 |
| Radix MKN | 256 KiB | delete | 1074.90 / 1697.20 | 634.30 / 929.15 | 631.80 / 927.55 |
| Radix MKN | 16 KiB | clean | 151.15 / 230.60 | 80.85 / 114.00 | 79.50 / 99.85 |
| Radix MKN | 16 KiB | delete | 150.80 / 243.90 | 78.65 / 121.10 | 79.75 / 125.15 |
| Original compact v3 | 256 KiB | clean | 1319.25 / 1583.45 | 1349.05 / 1602.50 | 4.55 / 1434.95 |
| Original compact v3 | 256 KiB | delete | 1270.55 / 1559.50 | 1294.05 / 1586.35 | 1294.95 / 1591.40 |
| Original compact v3 | 16 KiB | clean | 177.10 / 201.30 | 182.10 / 209.05 | 176.60 / 203.65 |
| Original compact v3 | 16 KiB | delete | 170.65 / 195.85 | 175.40 / 203.35 | 175.75 / 204.70 |

Known-word retention often exits early on clean inputs. Its latency improvement is partly a policy effect; the speed-only column isolates lookup changes.

## Runtime memory and traversal

| Runtime | Model object bytes | Prefix scratch bytes | Correction scratch bytes | Cursor object bytes | Max probe private commit MiB |
|---|---:|---:|---:|---:|---:|
| baseline | 48 | 3,152 | 8,536 | 96 | 8.36 |
| speed-only | 48 | 3,152 | 8,536 | 96 | 8.38 |
| final | 48 | 3,152 | 8,536 | 96 | 8.36 |

No C++ allocation-operator call was observed inside any measured query. Direct C allocation functions are not intercepted. Native process private commit includes the executable, model/index, preloaded queries and probe; object sizes are separate desktop-ABI subtotals. Cursor reuse adds caller-local cursor state, and the policy adds scalar scoring state. The cursor size is not a measurement of maximum stack usage. No expanded trie or persistent heap cache is introduced.

The final ESP32 compile/link checks report an 80-byte target cursor and 11,724 bytes for `Model + PrefixScratch + AutocorrectScratch`. These are target ABI object sizes, not hardware peak memory or stack measurements. The linked 886,592-byte test firmware includes a 16-byte introspection fixture; no hardware execution or latency measurement was performed.

| Radix artifact | Profile | Next-word checkpoints before / after | Physical record decodes before / after |
|---|---:|---:|---:|
| Radix counts | 256 KiB | 1,353,870 / 276,387 | 21,959,231 / 4,754,264 |
| Radix counts | 16 KiB | 519,829 / 111,752 | 9,839,244 / 2,245,093 |
| Radix MKN | 256 KiB | 819,839 / 299,444 | 13,800,090 / 5,427,795 |
| Radix MKN | 16 KiB | 282,831 / 90,230 | 3,908,599 / 1,661,934 |

Counters cover the 2,500 validation next-word queries, using separately instrumented frozen sources with identical output fingerprints. Instrumented timings are excluded.

## Correction quality

Policy: retain exact known words with finite score; require exactly one Unicode edit or adjacent transposition; require a 10% heuristic score-odds lead over the original spelling and runner-up candidate. The existing confidence threshold remains 0.3. Percentages below use the same 1,000 clean controls and 1,000 deletion typos within each panel. Wrong replacements are changed typos that do not match the expected word. All ordered completion outputs are unchanged.

| Panel | Artifact | Profile | Clean false changes % before → after | Wrong typo replacements % before → after | Successful recovery % before → after |
|---|---|---:|---:|---:|---:|
| validation | Radix counts | 256 KiB | 3.30 → 1.90 | 9.00 → 6.20 | 61.70 → 60.50 |
| validation | Radix counts | 16 KiB | 2.10 → 0.70 | 4.60 → 3.40 | 20.80 → 20.80 |
| validation | Radix MKN | 256 KiB | 3.10 → 2.50 | 4.90 → 4.10 | 52.80 → 52.10 |
| validation | Radix MKN | 16 KiB | 1.90 → 1.20 | 3.90 → 3.20 | 13.30 → 13.30 |
| validation | Original compact v3 | 256 KiB | 2.50 → 1.00 | 9.10 → 6.20 | 63.10 → 60.30 |
| validation | Original compact v3 | 16 KiB | 1.30 → 0.70 | 3.90 → 2.90 | 25.40 → 25.40 |
| exposed-regression | Radix counts | 256 KiB | 3.30 → 2.30 | 7.30 → 4.50 | 59.80 → 59.10 |
| exposed-regression | Radix counts | 16 KiB | 2.80 → 1.60 | 5.30 → 3.30 | 19.20 → 19.20 |
| exposed-regression | Radix MKN | 256 KiB | 2.10 → 2.00 | 5.10 → 4.30 | 49.20 → 48.90 |
| exposed-regression | Radix MKN | 16 KiB | 1.60 → 1.30 | 4.90 → 4.00 | 14.90 → 14.90 |
| exposed-regression | Original compact v3 | 256 KiB | 1.50 → 0.80 | 7.80 → 4.70 | 61.90 → 58.60 |
| exposed-regression | Original compact v3 | 16 KiB | 2.10 → 1.50 | 4.50 → 3.10 | 23.60 → 23.30 |
| confirmation | Radix counts | 256 KiB | 3.10 → 2.10 | 6.60 → 4.80 | 59.50 → 57.90 |
| confirmation | Radix counts | 16 KiB | 2.50 → 1.70 | 4.60 → 3.10 | 19.40 → 19.40 |
| confirmation | Radix MKN | 256 KiB | 2.40 → 2.10 | 5.60 → 4.70 | 51.30 → 50.30 |
| confirmation | Radix MKN | 16 KiB | 1.90 → 1.60 | 4.10 → 3.10 | 12.20 → 12.20 |
| confirmation | Original compact v3 | 256 KiB | 1.70 → 0.60 | 7.40 → 4.80 | 60.00 → 56.40 |
| confirmation | Original compact v3 | 16 KiB | 3.00 → 2.10 | 4.50 → 3.40 | 23.90 → 23.30 |

Fresh confirmation paired differences are candidate minus baseline in percentage points. Intervals use 2,000 resamples of whole content-hashed documents, preserving query pairing; 109 documents contribute correction queries. Lower false/wrong changes are better; higher recovery is better. These are descriptive intervals without a multiple-comparison adjustment.

| Artifact | Profile | Clean false-change delta pp [95% CI] | Wrong replacement delta pp [95% CI] | Recovery delta pp [95% CI] |
|---|---:|---:|---:|---:|
| Radix counts | 256 KiB | -1.000 [-1.677, -0.463] | -1.800 [-2.664, -1.033] | -1.600 [-2.259, -0.926] |
| Radix counts | 16 KiB | -0.800 [-1.386, -0.301] | -1.500 [-2.312, -0.854] | +0.000 [+0.000, +0.000] |
| Radix MKN | 256 KiB | -0.300 [-0.677, +0.000] | -0.900 [-1.534, -0.391] | -1.000 [-1.569, -0.470] |
| Radix MKN | 16 KiB | -0.300 [-0.690, +0.000] | -1.000 [-1.668, -0.468] | +0.000 [+0.000, +0.000] |
| Original compact v3 | 256 KiB | -1.100 [-1.834, -0.493] | -2.600 [-3.586, -1.720] | -3.600 [-4.752, -2.467] |
| Original compact v3 | 16 KiB | -0.900 [-1.538, -0.383] | -1.100 [-1.791, -0.579] | -0.600 [-1.107, -0.191] |

The 256 KiB radix-count model has 32.3% fewer clean false changes and 27.3% fewer wrong typo replacements on confirmation, with recovery falling 59.5% → 57.9%. The 16 KiB radix profiles preserve deletion recovery on this panel. MKN clean-false-change intervals touch zero. These observations do not establish a universally optimal policy.

## Full generation rerun

After all platform builds finished, the full prepared training input was processed again, with four workers, a 512 MiB requested training budget and a 2 GiB native job private-commit ceiling. Cold means an empty dedicated training cache with caching enabled; OS file caches were not flushed. Each invocation emits both profiles. Every NGTR/NGIX SHA-256 matches the fixed models used above. Training/encoding algorithms were unchanged; differences from historical timing are not attributed to lookup optimizations.

A read-only MKN-cold process sample found generator and parent affinity `0xFFFFFF` (all 24 logical CPUs), Normal priority, and 603.25 CPU seconds over 207.11 wall seconds. The benchmark job configures memory/kill-on-close limits, without CPU-rate or affinity limits; external ancestor job limits were not introspected. This rules out observed single-CPU inheritance from the microbenchmark. Generation timings are unpinned, single observations and are not a paired historical acceleration/regression experiment.

| Variant | Cache | Artifact-ready seconds | Peak native private MiB | 256 KiB profile combined bytes | 16 KiB profile combined bytes |
|---|---|---:|---:|---:|---:|
| Radix counts | cold | 412.442 | 89.12 | 262,139 | 16,379 |
| Radix counts | warm | 32.600 | 82.58 | 262,139 | 16,379 |
| Radix MKN | cold | 397.818 | 280.84 | 262,132 | 16,228 |
| Radix MKN | warm | 22.240 | 96.08 | 262,132 | 16,228 |

## Provenance

- Corpus manifest: `77c699da5c79b8602276f5f0d6556da8c89f5596e7d1d6de6976f48bcc3659e6`.
- Confirmation query panel: `929e95a911ba7d8a5b11379db983515cbe645829f18e0db2ea7f52a05f457c3e`.
- Candidate runtime/managed source manifest: `ab8567b53d4d927a85ab208524893e62fc429531cd7b7978997f9f39916569b9`.
- Candidate PredictionModel.cpp: `0ac65dfbf7f263a29822789f78770b33b073aff13be176c8efda10b117fe2b3b`.
- Candidate Utf8Fold.cpp: `0cb4c6abe2322ea94d52ef0cc8d8eb8442fd1933e618eba3b83c1eabdd672059`.
- Timing protocol manifest: `286b456a9d564cb2ba0bbfbd63a29ca981a54f34ea534a01e89f16a20b18c7c5`.
- Final generator executable: `85d7513e2f98258d55bede08722b18816ff147c4bcd08db7dd29990748d23db8`.

| Runtime | Microprofile executable SHA-256 |
|---|---|
| baseline | `53c63bf72e7db46f0176811e5d43feb534c79f2127ad88b7d99623088b6f1a52` |
| speed-only | `735b6360c34402f29cc8fe06a85bdaa6acdd2a588233ac0c6f1fe268f91acd8a` |
| final | `d4dc9f637824b8f3853af73e2dc3e996f6bc7bd1d4744d39a8d87df7facff824` |

Host: Intel Core i9-12900K, 16 cores/24 logical processors; Windows 11 Pro 10.0.26300; 68,494,786,560 bytes physical RAM; MSVC 14.51.36231 x64 Release; CMake 4.5.1; Python 3.11.9.

Retained artifacts under `build/radix-optimization`: source/binary/model snapshots and hashes, corpus verification, frozen panel manifest, every command and repeated latency sample, native outcome fingerprints, paired comparisons, separate traversal counters and full generation logs. See the adjacent README for the tools and measurement protocol.
