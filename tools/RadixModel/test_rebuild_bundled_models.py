"""Publication and provenance regressions using tiny authored NGTR models."""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("rebuild_bundled_models", Path(__file__).with_name("rebuild_bundled_models.py"))
REBUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REBUILD)
BUILDER = ROOT / "LVSerial/Widgets/AdvancedKeyboard/layout/build_ngtr_index.py"
if not BUILDER.is_file():
    BUILDER = ROOT / "tools/layout/build_ngtr_index.py"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def authored_v3(count):
    # Two independent roots, each spelling cat with the same exact count.
    # BFS records: root/root/c/c/a/a/t/t. No encoder under test is used.
    return struct.pack("<4sIII", b"NGTR", 3, 8, 0) + b"\0\2\0\2c\2c\2a\2a\2" + bytes([116, 1, count]) * 2


def authored_v4(count):
    # Two roots plus two compressed 'cat' edges; eight logical byte nodes.
    return struct.pack("<4sIII", b"NGTR", 4, 4, 8) + b"\0\10\0\10" + (b"\3cat\1" + bytes([count])) * 2


class RebuildTests(unittest.TestCase):
    def setUp(self):
        temporary_root = ROOT / "build/rebuild-bundled-tests"
        temporary_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="case-", dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.model_root = self.directory / "catalog/models"
        self.model_root.mkdir(parents=True)
        self.builder = REBUILD.read_builder(BUILDER)
        self.originals = {}
        records = []
        for language, size, count in [("en-US", 16, 9), ("fr-FR", 256, 7)]:
            data = authored_v3(count)
            name = f"{language}-{size}.ngtr"
            path = self.model_root / name
            path.write_bytes(data)
            self.originals[path] = data
            records.append(dict(id=f"corpus-{language}-{size}", language=language,
                                name=f"Authored {language} - {size} KiB", sourceName=f"original-{name}",
                                sourceLocale=language, sizeKiB=size, filename=name,
                                bytes=len(data), indexBytes=len(self.builder.build_index(data)),
                                nodeCount=8, sha256=sha(data), projectTrieId="old-content-id"))
        self.catalog = self.model_root.parent / "catalog.json"
        self.catalog.write_text(json.dumps(dict(version=1, source="Authored test fixtures", models=records)), encoding="utf8")
        self.compiled = self.directory / "CatalogData.inc"
        self.compiled.write_bytes(b"previous compiled catalog\n")
        self.builtin = self.directory / "builtin"
        self.builtin.mkdir()
        builtin_data = authored_v3(17)
        (self.builtin / "enUS16.ngtr").write_bytes(builtin_data)
        (self.builtin / "enUS16.ngix").write_bytes(self.builder.build_index(builtin_data))
        (self.builtin / "builtin.json").write_text(json.dumps(dict(
            id="builtin-en-us", language="en-US", name="Authored built-in", sizeKiB=16,
            filename="enUS16.ngtr", sourceName="Original supplied built-in", customNotice="keep this notice")), encoding="utf8")
        self.builtin_include = self.directory / "PredictionBuiltin.inc"
        self.builtin_include.write_bytes(b"previous builtin arrays\n")
        self.converter = self.directory / "fake-converter"
        self.converter.write_bytes(b"test converter placeholder, never executed")
        self.args = SimpleNamespace(catalog=self.catalog, converter=self.converter, index_builder=BUILDER,
                                    compiled_catalog=self.compiled, builtin_dir=self.builtin,
                                    builtin_include=self.builtin_include, work_dir=self.directory / "work", write=True)
        self.targets = [*self.originals, self.catalog, self.compiled, self.builtin_include,
                        self.builtin / "enUS16.ngtr", self.builtin / "enUS16.ngix", self.builtin / "builtin.json"]
        self.calls = []

    def snapshot(self):
        return {path: (path.read_bytes(), path.stat().st_mtime_ns) if path.exists() else None for path in self.targets}

    def fake_converter(self, command, **_):
        original = Path(command[1]).read_bytes()
        expected = {authored_v3(count): authored_v4(count) for count in (7, 9, 17)}
        converted = expected[original]
        Path(command[3]).write_bytes(converted)
        Path(command[4]).write_bytes(self.builder.build_index(converted))
        self.calls.append(command)
        return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

    def rebuild(self, converter=None):
        with mock.patch.object(REBUILD.subprocess, "run", side_effect=converter or self.fake_converter):
            return REBUILD.rebuild(self.args)

    def test_format_metadata_and_reserved_builtin_identity(self):
        result = self.rebuild()
        self.assertEqual(result["status"], "rebuilt")
        self.assertEqual(result["models"], 3)
        self.assertTrue(Path(result["backup"]).is_file())
        catalog = json.loads(self.catalog.read_text(encoding="utf8"))
        self.assertEqual(catalog["source"], "Authored test fixtures")
        for record, count, size in zip(catalog["models"], (9, 7), (16, 256)):
            data, original = authored_v4(count), authored_v3(count)
            self.assertEqual((self.model_root / record["filename"]).read_bytes(), data)
            self.assertEqual((record["formatVersion"], record["indexVersion"], record["nodeCount"], record["physicalNodeCount"]), (4, 2, 8, 4))
            self.assertEqual((record["sizeKiB"], record["bytes"], record["indexBytes"]), (size, len(data), len(self.builder.build_index(data))))
            self.assertEqual((record["sha256"], record["indexSHA256"]), (sha(data), sha(self.builder.build_index(data))))
            self.assertEqual((record["sourceFormatVersion"], record["sourceBytes"], record["sourceNodeCount"], record["sourceSHA256"]),
                             (3, len(original), 8, sha(original)))
        self.assertEqual(catalog["models"][0]["projectTrieId"], "trie-8ed4f951be18dfbc")
        self.assertIn("trie-8ed4f951be18dfbc", self.compiled.read_text())
        builtin = json.loads((self.builtin / "builtin.json").read_text())
        self.assertEqual((builtin["id"], builtin["projectTrieId"]), ("builtin-en-us", "builtin-en-us"))
        self.assertEqual(builtin["customNotice"], "keep this notice")
        self.assertEqual((self.builtin / "enUS16.ngtr").read_bytes(), authored_v4(17))
        self.assertEqual((self.builtin / "enUS16.ngix").read_bytes(), self.builder.build_index(authored_v4(17)))
        self.assertIn("kBuiltinTrie", self.builtin_include.read_text())
        self.assertIn("kBuiltinIndex", self.builtin_include.read_text())

    def test_dry_run_and_second_write_are_nonmutating(self):
        original = self.snapshot()
        self.args.write = False
        result = self.rebuild()
        self.assertEqual(result["status"], "planned")
        self.assertGreater(result["changedFiles"], 0)
        self.assertEqual(self.snapshot(), original)
        self.assertFalse((self.args.work_dir / "backups").exists())
        self.args.write = True
        self.rebuild()
        migrated = self.snapshot()
        conversion_calls = len(self.calls)
        result = self.rebuild()
        self.assertEqual((result["changedFiles"], result["backup"]), (0, None))
        self.assertEqual(len(self.calls), conversion_calls, "Already-v4 models should not invoke the converter")
        self.assertEqual(self.snapshot(), migrated, "Idempotent publication changed bytes or mtimes")

    def test_bad_converter_output_never_publishes_a_partial_catalog(self):
        for failure in ("version", "index"):
            with self.subTest(failure=failure):
                before = self.snapshot()
                def broken(command, **kwargs):
                    result = self.fake_converter(command, **kwargs)
                    if Path(command[1]).read_bytes() == authored_v3(17):
                        if failure == "version":
                            Path(command[3]).write_bytes(authored_v3(17))
                        else:
                            Path(command[4]).write_bytes(b"NGIX\2\0\0\0invalid")
                    return result
                with self.assertRaises(ValueError):
                    self.rebuild(broken)
                self.assertEqual(self.snapshot(), before)
                self.assertFalse((self.args.work_dir / "backups").exists())

    def test_edit_during_encoding_is_preserved(self):
        for name in ("model", "catalog", "compiled", "builtin-metadata", "builtin-include"):
            with self.subTest(target=name):
                watched = {"model": next(iter(self.originals)), "catalog": self.catalog,
                           "compiled": self.compiled, "builtin-metadata": self.builtin / "builtin.json",
                           "builtin-include": self.builtin_include}[name]
                before = self.snapshot()
                changed = False
                watched_count = {"builtin-metadata": 17, "model": 9}.get(name)
                def concurrent(command, **kwargs):
                    nonlocal changed
                    result = self.fake_converter(command, **kwargs)
                    current = Path(command[1]).read_bytes()
                    observed = watched_count is None or current == authored_v3(watched_count)
                    if not changed and observed:
                        watched.write_bytes(b"concurrent user edit")
                        changed = True
                    return result
                with self.assertRaisesRegex(ValueError, "changed.*preserved"):
                    self.rebuild(concurrent)
                self.assertEqual(watched.read_bytes(), b"concurrent user edit")
                after = self.snapshot()
                for path, value in before.items():
                    if path != watched:
                        self.assertEqual(after[path], value)
                watched.write_bytes(before[watched][0])

    def test_publish_stale_target_is_rejected_before_any_write(self):
        first, second = self.directory / "one", self.directory / "two"
        first.write_bytes(b"external edit"); second.write_bytes(b"old two")
        with mock.patch.object(REBUILD, "atomic_write") as writer:
            with self.assertRaisesRegex(ValueError, "changed.*preserved"):
                REBUILD.publish({first: b"new one", second: b"new two"}, {first: b"old one", second: b"old two"}, self.directory / "backups")
            writer.assert_not_called()
        self.assertEqual(first.read_bytes(), b"external edit")
        self.assertEqual(second.read_bytes(), b"old two")
        self.assertFalse((self.directory / "backups").exists())

    def test_publish_failure_rolls_back_existing_and_new_targets(self):
        for initially_present in (True, False):
            with self.subTest(initially_present=initially_present):
                first, second = self.directory / f"first-{initially_present}", self.directory / f"second-{initially_present}"
                old_first = b"old first" if initially_present else None
                if old_first is not None:
                    first.write_bytes(old_first)
                second.write_bytes(b"old second")
                real_write = REBUILD.atomic_write
                def fail_second(path, data):
                    if path == second:
                        raise OSError("injected publication failure")
                    real_write(path, data)
                backup_dir = self.directory / f"backups-{initially_present}"
                with mock.patch.object(REBUILD, "atomic_write", side_effect=fail_second):
                    with self.assertRaisesRegex(OSError, "injected"):
                        REBUILD.publish({first: b"new first", second: b"new second"}, {first: old_first, second: b"old second"}, backup_dir)
                self.assertEqual(first.read_bytes() if first.exists() else None, old_first)
                self.assertEqual(second.read_bytes(), b"old second")
                with zipfile.ZipFile(next(backup_dir.glob("*.zip"))) as archive:
                    manifest = json.loads(archive.read("manifest.json"))
                    self.assertEqual(len(manifest), 2)
                    for entry in manifest:
                        if entry["backup"]:
                            self.assertEqual(archive.read(entry["backup"]), old_first if entry["path"] == str(first) else b"old second")

    def test_mid_publish_edit_rolls_back_prior_write_and_keeps_user_edit(self):
        first, second = self.directory / "one", self.directory / "two"
        first.write_bytes(b"old one"); second.write_bytes(b"old two")
        real_write = REBUILD.atomic_write
        def race(path, data):
            real_write(path, data)
            if path == first and data == b"new one":
                second.write_bytes(b"external edit")
        with mock.patch.object(REBUILD, "atomic_write", side_effect=race):
            with self.assertRaisesRegex(ValueError, "changed.*preserved"):
                REBUILD.publish({first: b"new one", second: b"new two"}, {first: b"old one", second: b"old two"}, self.directory / "backups")
        self.assertEqual(first.read_bytes(), b"old one")
        self.assertEqual(second.read_bytes(), b"external edit")

    def test_rollback_does_not_overwrite_edit_to_already_published_target(self):
        first, second = self.directory / "one", self.directory / "two"
        first.write_bytes(b"old one"); second.write_bytes(b"old two")
        real_write = REBUILD.atomic_write
        def fail_after_external_edit(path, data):
            if path == second:
                first.write_bytes(b"external edit to completed target")
                raise OSError("injected publication failure")
            real_write(path, data)
        with mock.patch.object(REBUILD, "atomic_write", side_effect=fail_after_external_edit):
            with self.assertRaisesRegex(OSError, "injected"):
                REBUILD.publish({first: b"new one", second: b"new two"}, {first: b"old one", second: b"old two"}, self.directory / "backups")
        self.assertEqual(first.read_bytes(), b"external edit to completed target")
        self.assertEqual(second.read_bytes(), b"old two")


if __name__ == "__main__":
    unittest.main()
