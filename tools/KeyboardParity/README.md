# Keyboard parity

The C# exporter records model and paired-corpus hashes, correction candidates and
heuristic confidence, ranked predictions, folded words, sentence boundaries, and
GBUTest threshold regressions. The native runner replays those same inputs against
the canonical headless engine. No LVGL or firmware board drivers are required.
NGTR v2/v3/v4 models use the same interface. The existing English/QWERTY scorer
assumptions still apply; this tool is a parity check, not an accuracy benchmark.

From this repository root on Windows with .NET 8 and a C++17 developer shell:

```powershell
dotnet build tools/KeyboardParity/KeyboardParity.csproj -c Release
cmake -S tools/KeyboardParity -B build/keyboard-parity -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/keyboard-parity --parallel

# Supply your own existing paired file (alternating original/fuzzed lines).
$trie = 'path/to/model.ngtr'
$pairs = 'path/to/pairs.txt'
$out = 'build/keyboard-parity'
dotnet build/corpus-tools/bin/KeyboardParity/Release/net8.0-windows/KeyboardParity.dll $pairs $trie "$out/reference.tsv" "$out/queries.h"
python tools/layout/build_ngtr_index.py $trie "$out/model.ngix"
$meta = (Get-Content "$out/reference.tsv" -TotalCount 2 | Select-Object -Last 1) -split "`t"
if ((Get-FileHash $trie).Hash -ne $meta[1] -or (Get-FileHash $pairs).Hash -ne $meta[5]) { throw 'Reference input hashes differ' }
& "$out/host_parity.exe" $trie "$out/model.ngix" "$out/reference.tsv"
```

The runner checks correction decisions/confidence, normalized prefix order, and
each independently committed regression threshold. It exits with code 1 on a
mismatch. Verify the recorded file hashes before replay: the native runner checks
format/size/node metadata but does not independently hash the paired corpus.
Use `--config Release` and the configuration output directory for multi-config
CMake generators. Managed and native build products stay under ignored `build`.

After building both tools, run the small reproducible smoke check without a
corpus download:

```powershell
python tools/KeyboardParity/smoke_test.py --managed build/corpus-tools/bin/KeyboardParity/Release/net8.0-windows/KeyboardParity.dll --native build/keyboard-parity/host_parity.exe
```

It covers synthetic pairs with the checked-in v2, v3 and v4 models. To register
the same check with CTest, configure the native build with
`-DADVKBD_PARITY_ASSEMBLY=<absolute path to KeyboardParity.dll>`.

The exporter and replay algorithm were imported from the user's ESPOS tools;
only build paths, the native metadata version check and the current managed
Predict method signature were adapted. No corpora,
machine settings or separately licensed datasets are included. Existing ownership
is retained; no separate ESPOS-wide source license was supplied.
