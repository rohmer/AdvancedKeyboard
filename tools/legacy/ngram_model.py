#!/usr/bin/env python3
"""Train a frequency-weighted word n-gram model from Unicode text files."""

from __future__ import annotations

import argparse
import codecs
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass, field
import gzip
import hashlib
import io
from itertools import islice, zip_longest
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import time
import unicodedata
import zlib


WORD = re.compile(r"[^\W_]+(?:['’][^\W_]+)*")
NAMES = ("unigrams", "bigrams", "trigrams")
TRIE_HEADER = struct.Struct("<4sIII")
TRIE_NODE = struct.Struct("<IIHBB")
MIN_TRIE_SIZE = TRIE_HEADER.size + 2 * TRIE_NODE.size
UINT32_MAX = (1 << 32) - 1
CountedNgram = tuple[tuple[str, ...], int]
CACHE_VERSION = 1  # Increment when tokenization, sentence boundaries, or cache records change.


@dataclass
class TrieNode:
    label: int = 0
    mass: int = 0
    children: dict[int, TrieNode] = field(default_factory=dict)


def count_lines(lines: Iterable[str], limits: tuple[int | None, ...]) -> list[Counter]:
    """Count n-grams within each period-delimited segment of each line."""
    counts = [Counter() for _ in NAMES]
    for line in lines:
        for segment in line.casefold().split("."):
            tokens = WORD.findall(segment)
            for order, (counter, limit) in enumerate(zip(counts, limits), start=1):
                if limit != 0:
                    counter.update(zip(*(tokens[offset:] for offset in range(order))))
    return counts


def build_model(
    lines: Iterable[str], limits: tuple[int | None, ...], trie_size: int | None = None,
) -> dict:
    return model_from_counts(count_lines(lines, limits), limits, trie_size)


def model_from_counts(counts: list[Counter], limits: tuple[int | None, ...], trie_size: int | None) -> dict:
    """Rank retained n-grams by frequency and normalize their weights by prefix."""
    ranked = [
        sorted(counter.items(), key=lambda item: (-item[1], item[0])) if limit != 0 else []
        for counter, limit in zip(counts, limits)
    ]
    if trie_size is None:
        selections = [entries[:limit] for entries, limit in zip(ranked, limits)]
    else:
        selections = fit_trie_budget(ranked, limits, trie_size)

    model = {}
    for name, retained in zip(NAMES, selections):
        prefix_totals = Counter()
        for tokens, count in retained:
            prefix_totals[tokens[:-1]] += count
        model[name] = [
            {"tokens": tokens, "count": count, "weight": count / prefix_totals[tokens[:-1]]}
            for tokens, count in retained
        ]
    return model


def fit_trie_budget(
    ranked: list[list[CountedNgram]], limits: tuple[int | None, ...], trie_size: int,
) -> list[list[CountedNgram]]:
    remaining = min(UINT32_MAX, (trie_size - TRIE_HEADER.size) // TRIE_NODE.size) - 2
    root, sentence_root = TrieNode(), TrieNode()
    retained = [[] for _ in NAMES]
    targets = [min(limit or 0, len(entries)) for limit, entries in zip(limits, ranked)]

    # Try explicit targets first, then fill the remaining room across all enabled orders.
    for preferred in (True, False):
        candidates = [
            islice(entries, 0, target) if preferred else islice(entries, target, None)
            for entries, target in zip(ranked, targets)
        ]
        for group in zip_longest(*candidates):
            for order, entry in enumerate(group):
                if entry is None:
                    continue
                tokens, count = entry
                labels = " ".join(tokens).encode("utf-8")
                roots = (root, sentence_root) if order == 0 else (root,)
                needed = sum(missing_nodes(start, labels) for start in roots)
                if needed <= remaining and root.mass + count <= UINT32_MAX:
                    for start in roots:
                        insert_path(start, labels, count)
                    remaining -= needed
                    retained[order].append(entry)
    return retained


def missing_nodes(node: TrieNode, labels: bytes) -> int:
    for offset, label in enumerate(labels):
        child = node.children.get(label)
        if child is None:
            return len(labels) - offset
        node = child
    return 0


def write_trie(model: dict, path: Path) -> None:
    """Write a flat trie with contiguous child ranges and exact count masses."""
    root = TrieNode()
    sentence_root = TrieNode()
    node_count = 2
    for name, entries in model.items():
        for entry in entries:
            labels = " ".join(entry["tokens"]).encode("utf-8")
            node_count += insert_path(root, labels, entry["count"])
            if name == "unigrams":
                node_count += insert_path(sentence_root, labels, entry["count"])

    if max(node_count, root.mass) > UINT32_MAX:
        raise ValueError("trie exceeds the binary format's 32-bit count limit")

    with path.open("wb") as output:
        output.write(TRIE_HEADER.pack(b"NGTR", 2, node_count, TRIE_NODE.size))
        nodes = [root, sentence_root]
        for node in nodes:
            first_child = len(nodes) if node.children else 0
            output.write(TRIE_NODE.pack(
                first_child, node.mass, len(node.children), node.label, 0,
            ))
            nodes.extend(node.children[label] for label in sorted(node.children))


def insert_path(node: TrieNode, labels: bytes, count: int) -> int:
    nodes_added = 0
    node.mass += count
    for label in labels:
        if label not in node.children:
            node.children[label] = TrieNode(label)
            nodes_added += 1
        node = node.children[label]
        node.mass += count
    return nodes_added


def corpus_files(paths: Iterable[Path]) -> Iterable[Path]:
    for path in paths:
        if path.is_dir():
            files = sorted(
                entry for directory, _, names in os.walk(path, onerror=raise_walk_error)
                for name in names
                if (entry := Path(directory) / name).suffix.lower() == ".txt" and entry.is_file()
            )
            if not files:
                raise ValueError(f"no .txt files found in {path}")
            yield from files
        else:
            yield path


def raise_walk_error(error: OSError) -> None:
    raise error


class ProgressBar:
    def __init__(self, total: int) -> None:
        self.total = total
        self.next_update = 0.0
        self.previous = -1
        self.show(0)

    def show(self, percent: int) -> None:
        if percent != self.previous:
            filled = percent * 24 // 100
            bar = "#" * filled + "-" * (24 - filled)
            print(f"\r[{bar}] {percent:3d}%", end="", file=sys.stderr, flush=True)
            self.previous = percent

    def update(self, completed: int, source: io.BufferedReader | None = None) -> None:
        now = time.monotonic()
        if self.total and now >= self.next_update:
            self.next_update = now + 0.2
            position = completed if source is None else completed + source.tell()
            self.show(min(99, position * 100 // self.total))

    def close(self) -> None:
        print(file=sys.stderr, flush=True)


def read_corpus(
    paths: Iterable[Path], progress: ProgressBar | None = None, completed: int = 0,
) -> Iterable[str]:
    for path in paths:
        with path.open("rb") as source:
            prefix = source.peek(4)
            if prefix.startswith((codecs.BOM_UTF32_LE, codecs.BOM_UTF32_BE)):
                encoding = "utf-32"
            elif prefix.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)):
                encoding = "utf-16"
            else:
                encoding = "utf-8-sig"
            try:
                with io.TextIOWrapper(source, encoding=encoding) as corpus:
                    if progress is None:
                        yield from corpus
                    else:
                        for line in corpus:
                            yield line
                            progress.update(completed, source)
                        completed += source.tell()
            except UnicodeError as error:
                raise ValueError(f"cannot decode {path} as {encoding}: {error}") from error


def file_signature(stat: os.stat_result) -> list:
    return [CACHE_VERSION, unicodedata.unidata_version, stat.st_dev, stat.st_ino,
            stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns]


def load_counts(path: Path, signature: list, limits: tuple[int | None, ...]) -> list[Counter] | None:
    try:
        with gzip.open(path, "rt", encoding="utf-8") as source:
            if json.loads(source.readline()) != signature:
                return None
            counts = [Counter() for _ in NAMES]
            for line in source:
                tokens, count = json.loads(line)
                if (not isinstance(tokens, list) or not 1 <= len(tokens) <= len(NAMES)
                        or not all(isinstance(token, str) for token in tokens)
                        or type(count) is not int or count < 1):
                    return None
                if limits[len(tokens) - 1] != 0:
                    counts[len(tokens) - 1][tuple(tokens)] = count
            return counts
    except (OSError, EOFError, ValueError, TypeError, zlib.error):
        return None


def save_counts(path: Path, signature: list, counts: list[Counter]) -> None:
    with tempfile.NamedTemporaryFile(dir=path.parent, suffix=".tmp", delete=False) as temporary:
        temporary_path = Path(temporary.name)
    try:
        with gzip.open(temporary_path, "wt", encoding="utf-8", compresslevel=1) as output:
            output.write(json.dumps(signature) + "\n")
            for counter in counts:
                for entry in counter.items():
                    output.write(json.dumps(entry, ensure_ascii=False, separators=(",", ":")) + "\n")
        temporary_path.replace(path)
    finally:
        temporary_path.unlink(missing_ok=True)


def cached_counts(
    paths: Iterable[Path], cache_dir: Path, limits: tuple[int | None, ...], progress: ProgressBar | None,
) -> list[Counter]:
    cache_dir.mkdir(parents=True, exist_ok=True)
    combined = [Counter() for _ in NAMES]
    completed = 0
    for path in paths:
        key = hashlib.sha256(os.fsencode(os.path.normcase(path.resolve()))).hexdigest()
        cache_file = cache_dir / f"{key}.jsonl.gz"
        stat = path.stat()
        signature = file_signature(stat)
        counts = load_counts(cache_file, signature, limits)
        cache_miss = counts is None
        if cache_miss:
            counts = count_lines(read_corpus([path], progress, completed), (None,) * len(NAMES))
        if file_signature(path.stat()) != signature:
            raise ValueError(f"corpus file changed while processing: {path}")
        if cache_miss:
            save_counts(cache_file, signature, counts)
        for total, file_counts, limit in zip(combined, counts, limits):
            if limit != 0:
                total.update(file_counts)
        completed += stat.st_size
        if progress is not None:
            progress.update(completed)
    return combined


def nonnegative(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("expected a nonnegative integer")
    return number


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "corpus", nargs="*", type=Path,
        help="corpus files or directories searched recursively for .txt files "
             "(UTF-8, or UTF-16/UTF-32 with a BOM)",
    )
    parser.add_argument("-f", "--file", type=Path, help="read a single corpus file")
    parser.add_argument("-o", "--output", type=Path, help="JSON output file")
    cache_options = parser.add_mutually_exclusive_group()
    cache_options.add_argument("--cache-dir", type=Path, help="directory for per-file n-gram counts")
    cache_options.add_argument("--no-cache", action="store_true", help="count every corpus file again")
    parser.add_argument(
        "--trie-output", type=Path,
        help="binary trie output file (suppresses JSON on stdout)",
    )
    parser.add_argument(
        "--trie-size", type=nonnegative, metavar="BYTES",
        help=f"cap the binary trie size (minimum {MIN_TRIE_SIZE}); adjust n-gram counts to fit",
    )
    for name in NAMES:
        parser.add_argument(
            f"--{name}", type=nonnegative, metavar="N",
            help=f"keep the N most frequent {name} (default: all; 0: disable; "
                 "with --trie-size: N is a target)",
        )
    args = parser.parse_args()
    if args.trie_size is not None and args.trie_size < MIN_TRIE_SIZE:
        parser.error(f"--trie-size must be at least {MIN_TRIE_SIZE} bytes")
    if args.file is not None:
        if args.corpus:
            parser.error("use --file or positional corpus inputs")
        if not args.file.is_file():
            parser.error(f"--file must name a regular file: {args.file}")
    elif not args.corpus:
        parser.error("provide a corpus input or use --file")
    limits = tuple(getattr(args, name) for name in NAMES)
    progress = None
    try:
        paths = [args.file] if args.file is not None else list(corpus_files(args.corpus))
        for output_path in (args.output, args.trie_output):
            if output_path and output_path.exists():
                if any(output_path.samefile(path) for path in paths):
                    parser.error("output files must be different from every corpus file")
        if args.output and args.trie_output:
            if args.output.resolve() == args.trie_output.resolve() or (
                args.output.exists() and args.trie_output.exists()
                and args.output.samefile(args.trie_output)
            ):
                parser.error("JSON and trie outputs must use different files")
        if sys.stderr.isatty():
            progress = ProgressBar(sum(path.stat().st_size for path in paths))
        if args.no_cache:
            model = build_model(read_corpus(paths, progress), limits, args.trie_size)
        else:
            default_cache = Path(os.environ.get("LOCALAPPDATA") or Path.home() / ".cache") / "espos-ngram"
            counts = cached_counts(paths, args.cache_dir or default_cache, limits, progress)
            model = model_from_counts(counts, limits, args.trie_size)
        if args.trie_output:
            write_trie(model, args.trie_output)
        if args.output:
            with args.output.open("w", encoding="utf-8") as output:
                json.dump(model, output, ensure_ascii=False, indent=2)
                output.write("\n")
        elif not args.trie_output:
            print(json.dumps(model, indent=2))
        if progress is not None:
            progress.show(100)
    except (OSError, ValueError) as error:
        if progress is not None:
            progress.close()
            progress = None
        parser.exit(1, f"{parser.prog}: {error}\n")
    finally:
        if progress is not None:
            progress.close()


if __name__ == "__main__":
    main()
