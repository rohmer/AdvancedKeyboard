"""Import user-provided tries, rebuilding their encoding as NGTR v4/NGIX v2."""
from pathlib import Path
import argparse
import importlib.util
import json
import tempfile

PROFILES = [
    ("enUS", "en-US", "en-US", "English (US)"),
    ("esMS", "es-MX", "es-ES", "Spanish"),
    ("frFR", "fr-FR", "fr-FR", "French"),
    ("deDE", "de-DE", "de-DE", "German"),
    ("elEL", "el-EL", "el-GR", "Greek"),
    ("arAR", "ar-AR", "ar-SA", "Arabic"),
    ("hiHI", "hi-HI", "hi-IN", "Hindi"),
    ("zhZH", "zh-ZH", "zh-CN", "Mandarin Chinese"),
]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path, help="folder containing the original NGTR files")
    parser.add_argument("--converter", type=Path, required=True, help="built UICreatorRadixConvert executable")
    parser.add_argument("--work-dir", type=Path, help="staging and backup folder (default: build/keyboard-model-import)")
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = directory.parent.parent
    catalog_directory = root / "models/bundled"
    spec = importlib.util.spec_from_file_location("bundled_rebuild", root / "tools/RadixModel/rebuild_bundled_models.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    builder = tool.read_builder(root / "tools/layout/build_ngtr_index.py")
    work = (args.work_dir or root / "build/keyboard-model-import").resolve()
    work.mkdir(parents=True, exist_ok=True)
    records = []
    outputs, expected = {}, {}
    target = catalog_directory / "models"
    for path in [catalog_directory / "catalog.json"] + [
            target / f"{language}-{size}.ngtr" for _, _, language, _ in PROFILES for size in (16, 256)]:
        expected[path] = path.read_bytes() if path.exists() else None
    with tempfile.TemporaryDirectory(prefix="import-", dir=work) as temporary:
        for prefix, corpus_locale, language, name in PROFILES:
            for size in (16, 256):
                source = (args.corpus / f"{prefix}{size}.ngtr").resolve()
                original = source.read_bytes()
                expected[source] = original
                data, index = tool.encode(original, builder, args.converter.resolve(), Path(temporary) / f"{language}-{size}")
                filename = f"{language}-{size}.ngtr"
                record = dict(id=f"corpus-{language}-{size}", language=language,
                              name=f"{name} - {size} KiB", sourceName=source.name,
                              sourceLocale=corpus_locale, sizeKiB=size, filename=filename)
                records.append(tool.describe(record, original, data, index))
                outputs[target / filename] = data
    catalog = {"version": 1, "source": "User-provided NGTR files", "models": records}
    catalog["encoding"] = {"formatVersion": 4, "indexVersion": 2, "method": "lossless-reencode",
                           "profiles": "original nominal sizes; vocabulary and counts preserved"}
    outputs[catalog_directory / "catalog.json"] = tool.json_bytes(catalog)
    backup = tool.publish(outputs, expected, work / "backups")
    print(json.dumps({"models": len(records), "trieBytes": sum(record["bytes"] for record in records), "output": str(target), "backup": backup}))

if __name__ == "__main__":
    main()
