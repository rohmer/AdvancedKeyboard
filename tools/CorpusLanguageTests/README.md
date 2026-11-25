# Corpus language workflow checks

Run on Windows with the .NET 8 desktop SDK from this repository root:

```powershell
dotnet run --project tools/CorpusLanguageTests/CorpusLanguageTests.csproj -c Release -- . tools/NativeTrieGenerator/bin/UICreatorModelGen.exe
```

The first argument identifies this standalone repository, whose original model
fixtures live in `tests/fixtures/language-models`. Omit the second argument for
managed-only checks. Tests keep temporary data under `build/corpus-language-tests`
and use a local HTTP server; they do not access external websites or alter user
corpora/settings. No OpenNLP models or Java downloads are required.

The suite covers original byte-exact NGTR goldens, all supported Unicode
languages, RAM/disk generation parity, dictionary filters, UI/default settings,
offline crawling/Wikipedia exports and native helper generation/cancellation.
The keyboard simulator tests additionally check native/managed correction
policy parity with `--policy-probe <radix_prediction executable>`.
