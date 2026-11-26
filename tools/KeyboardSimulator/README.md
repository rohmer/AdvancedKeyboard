# Keyboard simulator

Build/run the Windows .NET 8 simulator from this repository root:

```powershell
dotnet run --project tools/KeyboardSimulator/KeyboardSimulator.csproj -c Release
```

The shared project files load `tools/layout` through `tools/Directory.Build.targets`.
Definitions are copied to output `Keyboards` directories. No Creator or ESPOS
checkout is required. Arabic/Urdu support RTL display; Hindi/Bengali use InScript,
Russian uses JCUKEN, Indonesian uses QWERTY, and Mandarin supports shared Pinyin
character composition. Pinyin's dictionary notice is `PinyinData.LICENSE.txt`.

Enable **Numeric** in the toolbar to simulate the device's 4×3 numeric keypad:
`1 2 3 / 4 5 6 / 7 8 9 / . 0 delete`. It accepts ASCII digits and one decimal
point, including when typing or pasting into the preview. Deleting and replacing
selected text work normally. Suggestions, autocorrect and Pinyin are paused in
numeric mode; turn Numeric off to return to the selected language and loaded trie.

Load legacy NGTR v2, compact v3 or compressed radix v4 models. A matching radix
index is validated when present. Counts and optional smoothed scores use the
same semantics as the portable runtime. Automatic corrections preserve exact
known finite-score words and require one scalar edit/transposition plus a 10%
heuristic evidence lead over the original and next candidate. The user's
confidence threshold still applies; manual completion order is unchanged.

```powershell
dotnet run --project tools/KeyboardSimulator.Tests/KeyboardSimulator.LanguageTests.csproj -c Release -- tools/layout --native tools/NativeTrieGenerator/bin/UICreatorModelGen.exe
```

Add `--policy-probe <native radix_prediction executable>` after building the
native tests to compare exact native/managed correction decisions/confidence.
The test suite covers language input, Unicode spans, legacy and v4 reader/index
parity, prediction order, correction safety and malformed-model rejection.
