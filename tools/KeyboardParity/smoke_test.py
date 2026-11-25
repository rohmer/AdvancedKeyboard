"""Replay deterministic synthetic pairs through managed/native NGTR v2/v3/v4 readers."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--managed", required=True, type=Path, help="Built KeyboardParity.dll")
    parser.add_argument("--native", required=True, type=Path, help="Built host_parity executable")
    args = parser.parse_args()
    for path in (args.managed, args.native):
        if not path.is_file():
            parser.error(f"Build the parity tool first: {path}")
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="advanced-keyboard-parity-") as name:
        work = Path(name)
        pairs = work / "pairs.txt"
        pairs.write_text("the quick brown fox.\nteh quick brown fox.\nin the world.\nin teh world.\nhello world.\nhellp world.\n", encoding="utf-8")
        models = (root / "tests/fixtures/language-models/legacy.ngtr",
                  root / "tests/fixtures/language-models/full.ngtr",
                  root / "models/bundled/models/en-US-16.ngtr")
        for version, model in zip((2, 3, 4), models):
            reference, index = work / f"v{version}.tsv", work / f"v{version}.ngix"
            subprocess.run(["dotnet", str(args.managed.resolve()), str(pairs), str(model), str(reference)], check=True)
            metadata = reference.read_text().splitlines()[1].split("\t")
            if (metadata[1].lower() != hashlib.sha256(model.read_bytes()).hexdigest() or
                    metadata[5].lower() != hashlib.sha256(pairs.read_bytes()).hexdigest()):
                raise RuntimeError("Reference input hashes differ")
            subprocess.run([sys.executable, str(root / "tools/layout/build_ngtr_index.py"), str(model), str(index)], check=True)
            subprocess.run([str(args.native.resolve()), str(model), str(index), str(reference)], check=True)
            print(f"NGTR v{version} managed/native parity passed", flush=True)


if __name__ == "__main__":
    main()
