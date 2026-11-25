from pathlib import Path
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PACKER = ROOT / "tools/models/pack_ngram_partition.py"
LEGACY = ROOT / "tools/legacy/ngram_model.py"
SPEC = importlib.util.spec_from_file_location("partition_test", PACKER)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class LegacyToolTests(unittest.TestCase):
    def test_original_python_trainer_is_preserved_byte_for_byte(self):
        record = next(item for item in json.loads((ROOT / "PROVENANCE/espos-portable-tools.json").read_text())["files"]
                      if item["target"] == "tools/legacy/ngram_model.py")
        self.assertEqual(hashlib.sha256(LEGACY.read_bytes()).hexdigest(), record["sha256"])

    def test_legacy_cli_still_generates_v2_and_json(self):
        with tempfile.TemporaryDirectory() as name:
            work = Path(name)
            source = work / "corpus.txt"
            source.write_text("Hello world. Hello reader.\n", encoding="utf-8")
            trie, counts = work / "model.ngtr", work / "model.json"
            result = subprocess.run([sys.executable, str(LEGACY), "--file", str(source), "--no-cache",
                                     "--trie-output", str(trie), "--output", str(counts)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(struct.unpack_from("<4sI", trie.read_bytes()), (b"NGTR", 2))
            unigrams = json.loads(counts.read_text())["unigrams"]
            self.assertEqual({tuple(item["tokens"]): item["count"] for item in unigrams},
                             {("hello",): 2, ("world",): 1, ("reader",): 1})

    def test_partition_supports_all_native_model_formats(self):
        models = (ROOT / "tests/fixtures/language-models/legacy.ngtr",
                  ROOT / "tests/fixtures/language-models/full.ngtr",
                  ROOT / "models/bundled/models/en-US-16.ngtr")
        for path in models:
            with self.subTest(model=str(path)):
                model = path.read_bytes()
                image = MODULE.pack_partition(model, 0x20000)
                magic, version, model_size, index_size, digest = struct.unpack_from("<4sIII32s", image)
                self.assertEqual((magic, version, model_size, digest),
                                 (b"NGPK", 1, len(model), hashlib.sha256(model).digest()))
                self.assertEqual(image[48:48 + model_size], model)
                self.assertEqual(image[48 + model_size:], MODULE.build_index(model))
                self.assertEqual(len(image), 48 + model_size + index_size)

    def test_bad_input_and_small_partition_do_not_replace_output(self):
        with tempfile.TemporaryDirectory() as name:
            work = Path(name)
            source, output = work / "model.ngtr", work / "image.bin"
            for model, budget in ((b"broken", "0x20000"),
                                  ((ROOT / "models/bundled/models/en-US-16.ngtr").read_bytes(), "1"),
                                  (bytes(65537), "0x20000")):
                source.write_bytes(model)
                output.write_bytes(b"previous image")
                result = subprocess.run([sys.executable, str(PACKER), str(source), str(output), budget], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output.read_bytes(), b"previous image")
                self.assertEqual(source.read_bytes(), model)
            self.assertFalse(list(work.glob("*.tmp")))

    def test_input_output_alias_is_rejected_without_changing_model(self):
        with tempfile.TemporaryDirectory() as name:
            source = Path(name) / "model.ngtr"
            data = (ROOT / "models/bundled/models/en-US-16.ngtr").read_bytes()
            source.write_bytes(data)
            result = subprocess.run([sys.executable, str(PACKER), str(source), str(source), "0x20000"], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(source.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
