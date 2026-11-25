"""Pack an NGTR v2/v3/v4 model and canonical NGIX into an NGPK1 flash image."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import tempfile

_INDEX_PATH = Path(__file__).resolve().parents[1] / "layout/build_ngtr_index.py"
_SPEC = importlib.util.spec_from_file_location("advanced_keyboard_partition_index", _INDEX_PATH)
_INDEX = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_INDEX)
build_index = _INDEX.build_index


def pack_partition(model: bytes, partition_size: int) -> bytes:
    """Keep the original ESPOS NGPK1 framing and 64 KiB input-model limit."""
    if len(model) > 64 * 1024:
        raise ValueError("the ESPOS trie partition requires a compact model of at most 64 KiB")
    index = build_index(model)
    header = struct.pack("<4sIII32s", b"NGPK", 1, len(model), len(index), hashlib.sha256(model).digest())
    image = header + model + index
    if len(image) > partition_size:
        raise ValueError("the model and index exceed the ngram partition size")
    return image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("partition_size", type=lambda value: int(value, 0))
    args = parser.parse_args()
    temporary = None
    try:
        if args.model.resolve() == args.output.resolve() or (
            args.output.exists() and args.model.samefile(args.output)
        ):
            raise ValueError("model and partition image must use different files")
        with args.model.open("rb") as model:
            image = pack_partition(model.read(64 * 1024 + 1), args.partition_size)
        with tempfile.NamedTemporaryFile(dir=args.output.parent, prefix=args.output.name + ".", suffix=".tmp", delete=False) as output:
            temporary = Path(output.name)
            output.write(image)
        os.replace(temporary, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
