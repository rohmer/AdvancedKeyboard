# Source ownership and third-party notices

This extraction preserves the existing source ownership and license notices.
The root `LICENSE` is the LVGL UI Creator MIT license for its authored code; it
does not replace the terms of imported assets or grant new rights to inputs.

- The portable widget and prediction code were originally ported from the
  user's ESPOS project. `src/PROVENANCE.md` records that history and the absence
  of a separately supplied project-wide ESPOS license. Ownership remains as
  stated in those sources; this extraction does not relicense them.
- Keyboard glyph fonts retain their adjacent `src/Keyboard*Font.LICENSE.txt`
  notices, including Bitstream/DejaVu/Arev and SIL Open Font License terms.
- Pinyin data retains `src/PinyinData.LICENSE.txt`; the English model and
  dictionary notices are under `src/models`.
- Native and managed vocabulary notices are under
  `tools/NativeTrieGenerator/source/wordlists` and `tools/NgramTrie/Wordlist`.
- The user-supplied language models retain `models/bundled/README.md` and the
  exact catalog hashes. No text corpus, downloaded Wikipedia dump or new
  training-data license is included or implied.
- Test-only nlohmann JSON, when enabled, has its MIT notice under
  `tests/third_party/nlohmann`. Managed package dependencies are declared by
  their existing pinned NuGet references and restored separately.

LVGL is an external dependency with its own license. Generated device builds
use the checked-in glyphs/layouts/tables and do not need font or language-model
training tools. Optional font regeneration downloads only explicitly pinned
font sources, verifies their checksums, and requires an external converter
selected with `--converter`.
