# Native language-model and keyboard benchmarks

These tools operate on this standalone repository. They do not look for a
Creator or ESPOS checkout. The native probes compile the actual prediction
sources under `src`, with public headers under `include`; offline conversion
uses `tools/ModelCodec`. Python 3.10+ and a C++17 compiler are required.

Build the probes from a compiler developer prompt on Windows, or GCC/Clang on
Linux:

```sh
cmake -S tools/benchmarks -B build/benchmarks -DCMAKE_BUILD_TYPE=Release
cmake --build build/benchmarks --config Release --parallel
python -m unittest discover -s tools/benchmarks -v
```

This produces `PredictionProbe`, `RuntimeProfile` and `RadixConvert`. Windows
executables have `.exe`; multi-configuration generators may place them under
`Release`. To measure preserved sources, pass explicit `KEYBOARD_CORE`,
`KEYBOARD_PUBLIC_INCLUDE` and `KEYBOARD_CODEC` CMake paths. The default is always
this repository, not an adjacent checkout. The native corpus generator can be
built separately from `tools/NativeTrieGenerator`; see its README.

## Read-only corpus preparation

```sh
python tools/benchmarks/benchmark.py prepare --corpus /path/to/fr-FR --output build/benchmark/prepared
python tools/benchmarks/benchmark.py queries --prepared build/benchmark/prepared
```

The parser expects TXT inputs and the length-prefixed JSON `.crawl` sidecars.
Normalized page bodies receive a deterministic 80/10/10 split. Identical page
bodies stay together; exact held-out sentence overlaps are purged globally from
training, including Wikipedia. Training sentences are deduplicated. The manifest
records source/derived hashes, byte counts and the split seed. Sources are never
written. Keep derived data under ignored `build`, not inside the corpus.

Wikipedia TXT exports discard article IDs and boundaries. The resulting holdout
uses crawl documents; it does not claim an article-level Wikipedia split or
semantic/paraphrase deduplication. The optional hash sample spans the whole
corpus and is labelled separately from full-corpus results.

## Generation and prediction

```sh
python tools/benchmarks/matrix.py --prepared build/benchmark/prepared --baseline /path/to/frozen-ModelGen --optimized /path/to/new-ModelGen --baseline-probe /path/to/frozen-PredictionProbe --optimized-probe /path/to/new-PredictionProbe --wordlists tools/NativeTrieGenerator/source/wordlists --index-builder tools/layout/build_ngtr_index.py --output build/benchmark/full
```

Executables are copied into hash-addressed snapshots before measurement. The
runner records every command, stage timing, output hash and memory measurement.
Cold means an empty dedicated training cache with caching enabled; warm reuses
that cache. OS file caches are not flushed. Four workers, a 512 MiB requested
optimized training budget and a 2 GiB native job private-commit ceiling are the
default. Windows reports job peak private committed bytes; Linux samples native
child `VmHWM` RSS and checks its threshold. These are different metrics and
enforcement mechanisms. A separate Python index builder's memory is outside
the native job, although its elapsed time contributes to artifact-ready time.

The probe calls real `Model`, `Suggest` and `BestAutocorrection` APIs. French
queries use the native keyboard normalizer. Prefixes of zero through three
Unicode scalars are always incomplete. Reports separate all-query and
in-vocabulary top-1/top-3/MRR, coverage, clean false changes, wrong typo changes,
successful deletion recovery and native p50/p95 latency. Default acceptance
threshold is 0.3. Calibrated log loss is unavailable for legacy priority fallback
and must not be compared across differing vocabulary/OOV mappings.

Legacy profile budgets constrain NGTR alone; radix budgets include NGTR+NGIX.
Use `equal_storage.py` to regenerate cached profiles at matching combined sizes,
and `finish_analysis.py` for format-only conversion and paired comparisons.
`compare.py` checks query/outcome hashes and resamples whole content-hashed
documents 2,000 times. Its ordered fingerprints cover words, not all numeric
score metadata; native unit tests cover the latter.

## Runtime and correction changes

Use validation only for development. After a test panel has been published,
label it exposed regression. Before further changes, derive and freeze a new
confirmation panel from held-out documents absent from every old test header:

```sh
python tools/benchmarks/prepare_runtime_confirmation.py --prepared build/benchmark/prepared --output build/benchmark/confirmation
```

Do not evaluate confirmation until runtime/policy freeze. Its documents still
come from the same crawl, not a new independent corpus. Synthetic one-deletion
typos do not represent every user typing error.

`RuntimeProfile MODEL INDEX QUERIES.tsv [LOGICAL_CPU]` preloads/normalizes inputs,
warms one complete pass and measures the next. On Windows/Linux its optional
CPU parameter pins the calling thread. It observes C++ allocation operators
only inside the native call; it does not intercept direct C allocation or claim
hardware heap/stack maxima. Keep other builds and heavy I/O idle while timing.

```sh
python tools/benchmarks/runtime_matrix.py --runtime baseline=/path/to/before-RuntimeProfile --runtime speed-only=/path/to/speed-RuntimeProfile --runtime final=/path/to/after-RuntimeProfile --models build/benchmark/models.json --queries build/benchmark/prepared/validation.tsv --output build/benchmark/runtime --repetitions 8 --cpu 2
```

The model manifest is a list of `{variant, profileBytes, model, index,
modelSHA256, indexSHA256}` records, with absolute model/index paths. The runner
checks hashes, rotates/reverses runtime order and reports medians and min/max
ranges of each run's p50/p95. Query/model loading is outside call latency;
process private memory includes the probe and loaded inputs. Scratch/cursor
object sizes are separate ABI subtotals, not maximum stack measurements.

After freeze, `runtime_quality.py` compares validation, exposed regression and
confirmation with fixed model bytes and paired document-bootstrap intervals.
Report false changes, wrong replacements and recovery together: stricter gates
can suppress useful corrections too. The task-specific `publish_results.py`
and `publish_runtime_results.py` validate retained provenance before publishing
their complete experiment tables.

Historical French reports are retained for reference. Their measurements and
paths describe the original Creator/ESPOS experiments; no corpus, credentials,
settings, executables or ignored experiment artifacts are bundled here. They do
not benchmark this extraction or establish ESP32 hardware latency. Full-corpus
benchmarking is explicitly invoked and never runs as part of CTest.
