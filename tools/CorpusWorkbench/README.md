# ESPOS Corpus Workbench

Corpus Workbench brings the corpus and n-gram tools into one Windows application. Use the navigation on the left to switch tools. Running work continues when you switch pages, and closing the application waits for active work to stop and save its progress.

- **Collect corpora** contains the web crawler and Wikipedia XML extractor. The crawler accepts multiple URLs, saves one corpus per site, filters complete sentences, and supports stop and resume. The Wikipedia extractor reads compressed XML dumps.
- **Build n-grams** contains the trie generator, including language word lists, directory recursion, shared RAM budgets, count/training caches, radix or compatibility formats, scoring choices and batch size profiles.
- **Test corrections** contains the sentence fuzzer, GBU scorer and threshold regression, and keyboard simulator with its trie viewer.

After collecting text, open **Trie generator** and click **Use collected corpora** to select the exact collected files in the latest corpus's language. You can add more files or folders in the generator. Other languages are not included merely because their files share an output folder. It accepts English, Spanish, French, German, Greek, Arabic, Hindi, Mandarin Chinese, Bengali, Indonesian, Urdu and Russian. The original five vocabulary filters are preserved; the seven new languages default to NFC Unicode text without a bundled filter. Mandarin uses Han character n-grams. A completed trie becomes available through **Use latest trie** in the fuzzer, tester, and keyboard simulator. Completed sentence pairs are available through **Use latest fuzzed file** in the tester. Hover over these buttons to see their file paths. The workbench remembers these completed files and the last selected tool between sessions. The generator also retains its own settings.

Fresh settings use **Compressed radix (NGTR v4) + Counts**, a 262,144-byte combined model/index budget and a 512 MiB RAM workspace shared by active workers. Saved explicit settings retain their behavior. Compatibility v3/v2 exports remain available with Counts scoring and historical NGTR-only byte caps. Radix budgets include the `.ngix` lookup index; keep it beside the `.ngtr` file when deploying. Additional sizes, such as `16384,262144`, reuse counting/training and create `<stem>-<bytes>.ngtr` siblings. Batch profiles are available only for radix, and disabled compatibility controls preserve their saved values.

**Modified Kneser-Ney** is an explicit radix scoring option. It trains interpolated trigram probabilities and normalizes backoff after pruning. French validation showed some larger-profile next-word gains, alongside small-profile vocabulary/completion and correction costs. It adds training/metadata work and is not universally more accurate; benchmark the language and profile you intend to deploy. Counts stays the default. `tools/benchmarks/README.md` describes the evaluation method.

The generation-engine label reports the available backend and full helper path. Put `UICreatorModelGen.exe` beside the application, build it under an ancestor's `tools/NativeTrieGenerator/bin`, or set `UICREATOR_MODELGEN` to its executable. The new GUI format/scoring, RAM and batch controls require a current native helper, which is checked for supported capabilities. Missing or outdated helpers produce a clear error. The managed C# CLI remains available for its original options; failed native runs do not silently start a second full generation. Progress and Stop remain connected to the UI. Temporary counts use local storage, and completed output files are replaced atomically individually; a batch/model-index pair is not one filesystem transaction.

The crawler and Wikipedia extractor offer all twelve generation languages plus Portuguese. The seven new profiles clearly report a Unicode sentence-boundary fallback without OpenNLP/POS grammar classification. They work without Java. The original six collection profiles retain their OpenNLP models. See [Corpus Crawler](../CorpusCrawler/README.md) and [Wikipedia Corpus](../WikipediaCorpus/README.md).

The simulator selects the keyboard for a trie generated in the workbench. The fuzzer and GBU tester use their existing English spelling data and QWERTY neighbor model.

Open `CorpusWorkbench.csproj` in Visual Studio with the **.NET desktop development** workload and the **.NET 8 SDK** installed. Its relative project references include all supporting tools. Run **CorpusWorkbench** as the startup project with **F5**. NuGet restores the project dependencies, and the build copies the required keyboard definitions, spelling data, and any configured OpenNLP assets into the application output.

Build the solution from the workspace root on the command line with the .NET 8 Windows desktop SDK.

```powershell
dotnet build tools/CorpusWorkbench/CorpusWorkbench.csproj -c Release
```

Run `build/corpus-tools/bin/CorpusWorkbench/Release/net8.0-windows/CorpusWorkbench.exe` from this repository. Keep the output folder together so keyboard definitions, spelling data, any configured OpenNLP assets and runtime dependencies are available. The crawler and Wikipedia exporter use the existing [OpenNLP setup](../CorpusCrawler/README.md) only for their original six model-based profiles.

Build the native helper separately as described in the adjacent `NativeTrieGenerator/README.md`, or use the helper bundled by a desktop build.
