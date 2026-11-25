"""Validate NGTR v2/v3/v4 and build its directly mapped NGIX lookup index."""

from __future__ import annotations

import argparse
from array import array
from pathlib import Path
from struct import Struct


HEADER = Struct("<4sIII")
CHECKPOINT = Struct("<III")
NODES_PER_CHECKPOINT = 32
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT16_MIN = -(1 << 15)
SCORE = Struct("<h")


def read_var_uint32(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 32, 7):
        if offset == len(data):
            raise ValueError("truncated NGTR record")
        byte = data[offset]
        offset += 1
        if shift == 28 and byte > 0x0F:
            raise ValueError("NGTR variable-length integer exceeds 32 bits")
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            if shift != 0 and byte == 0:
                raise ValueError("noncanonical NGTR variable-length integer")
            return value, offset
    raise ValueError("invalid NGTR variable-length integer")


def append_var_uint32(output: bytearray, value: int) -> None:
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)


def read_var_uint64(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 64, 7):
        if offset == len(data): raise ValueError("truncated NGTR 64-bit count")
        byte = data[offset]; offset += 1
        if shift == 63 and byte > 1: raise ValueError("NGTR count exceeds 64 bits")
        value |= (byte & 127) << shift
        if byte < 128:
            if shift and byte == 0: raise ValueError("noncanonical NGTR count")
            return value, offset
    raise ValueError("invalid NGTR 64-bit count")


def _validate_v2(data: bytes, count: int, stride: int) -> bytearray:
    if count < 2 or stride != 12 or len(data) != 16 + count * 12: raise ValueError("invalid NGTR v2 length")
    record = Struct("<IIHBB")
    following = 2
    for node in range(count):
        start, mass, degree, label, reserved = record.unpack_from(data, 16 + node * 12)
        if node >= following or reserved or degree > 255 or (label == 0) != (node < 2): raise ValueError("invalid NGTR v2 record")
        if degree and start != following or not degree and start: raise ValueError("invalid NGTR v2 child range")
        if start + degree > count: raise ValueError("invalid NGTR v2 child range")
        remaining = mass; previous = 0
        for child in range(start, start + degree):
            _, amount, _, character, _ = record.unpack_from(data, 16 + child * 12)
            if character <= previous or amount > remaining: raise ValueError("invalid NGTR v2 subtree")
            previous = character; remaining -= amount
        if node < 2 and remaining or node >= 2 and not degree and not remaining: raise ValueError("invalid NGTR v2 terminal")
        following += degree
    if following != count: raise ValueError("invalid NGTR v2 node count")
    return bytearray()  # The fixed-record format already provides random access.


def _build_radix_index(data: bytes, count: int, logical_count: int) -> bytearray:
    if count < 2 or count > 0xFFFFFF or count > logical_count or logical_count > 2 + (count - 2) * 256:
        raise ValueError("invalid NGTR v4 node counts")
    if len(data) - 16 < count * 2: raise ValueError("NGTR v4 length does not match its nodes")
    mass_start = 16 + ((count + 31) // 32) * 12
    output = bytearray(mass_start)
    HEADER.pack_into(output, 0, b"NGIX", 2, len(data), count)
    children = array("I", [0]) * count
    masses = array("Q", [0]) * count
    degrees = bytearray(count); labels = bytearray(count)
    bounds = array("h", [INT16_MIN]) * count
    cursor = 16; following = 2; expanded = 2; scored = False
    for node in range(count):
        if not node % 32: CHECKPOINT.pack_into(output, 16 + node // 32 * 12, cursor, following, 0)
        length, cursor = read_var_uint32(data, cursor)
        if length > 256 or (length == 0) != (node < 2) or cursor + length > len(data): raise ValueError("invalid NGTR v4 label span")
        span = data[cursor:cursor + length]; cursor += length
        if b"\0" in span: raise ValueError("NUL in NGTR v4 label span")
        if node >= 2: expanded += length
        flags, cursor = read_var_uint32(data, cursor); degree = flags >> 3
        if node >= following or degree > 255 or following + degree > count: raise ValueError("invalid NGTR v4 child range")
        terminal = 0
        if flags & 1:
            terminal, cursor = read_var_uint64(data, cursor)
            if node < 2 or not terminal: raise ValueError("invalid NGTR v4 terminal")
        probability = INT16_MIN
        if flags & 2:
            if cursor + 2 > len(data): raise ValueError("truncated NGTR v4 probability")
            probability = SCORE.unpack_from(data, cursor)[0]; cursor += 2
            if probability > 0: raise ValueError("NGTR v4 probability exceeds one")
        if flags & 4:
            if cursor + 2 > len(data): raise ValueError("truncated NGTR v4 backoff")
            cursor += 2
        if node == 0:
            scored = bool(flags & 2)
            if flags & 4 or scored and probability != 0: raise ValueError("invalid NGTR v4 scoring tag")
        elif node == 1:
            if flags & 4 or bool(flags & 2) != scored: raise ValueError("invalid NGTR v4 unknown probability")
        elif flags & 6 and not scored or scored and terminal and not flags & 2:
            raise ValueError("NGTR v4 scoring metadata disagrees with its tag")
        if node >= 2 and not degree and not terminal and not flags & 6: raise ValueError("empty NGTR v4 leaf")
        labels[node] = span[0] if span else 0; degrees[node] = degree; children[node] = following; masses[node] = terminal
        if node >= 2 and flags & 2: bounds[node] = probability
        following += degree
    if following != count or cursor != len(data) or expanded != logical_count: raise ValueError("NGTR v4 length/count mismatch")
    for node in range(count - 1, -1, -1):
        previous = 0; mass = masses[node]; bound = bounds[node]
        for child in range(children[node], children[node] + degrees[node]):
            if labels[child] <= previous: raise ValueError("unordered NGTR v4 children")
            previous = labels[child]; mass += masses[child]; bound = max(bound, bounds[child])
        if mass > UINT64_MAX: raise ValueError("NGTR v4 aggregate frequency exceeds 64 bits")
        masses[node] = mass; bounds[node] = bound
    for node in range(count):
        if not node % 32: Struct("<I").pack_into(output, 16 + node // 32 * 12 + 8, len(output) - mass_start)
        if degrees[node]:
            append_var_uint32(output, masses[node])  # ULEB loop is width-independent.
            if scored: output.extend(SCORE.pack(bounds[node]))
    return output


def build_index(data: bytes) -> bytearray:
    if len(data) < HEADER.size or len(data) > UINT32_MAX:
        raise ValueError("invalid NGTR file length")
    magic, version, node_count, record_size = HEADER.unpack_from(data)
    if magic == b"NGTR" and version == 2: return _validate_v2(data, node_count, record_size)
    if magic == b"NGTR" and version == 4: return _build_radix_index(data, node_count, record_size)
    if magic != b"NGTR" or version != 3 or record_size != 0:
        raise ValueError("expected a compact NGTR v3 model")
    if node_count < 2 or not 2 * node_count <= len(data) - HEADER.size <= 8 * node_count:
        raise ValueError("NGTR file length does not match the node count")

    checkpoint_count = (node_count + NODES_PER_CHECKPOINT - 1) // NODES_PER_CHECKPOINT
    mass_start = HEADER.size + checkpoint_count * CHECKPOINT.size
    output = bytearray(mass_start)
    HEADER.pack_into(output, 0, b"NGIX", 1, len(data), node_count)

    child_starts = array("I", [0]) * node_count
    masses = array("I", [0]) * node_count
    child_counts = bytearray(node_count)
    labels = bytearray(node_count)
    offset = HEADER.size
    next_child = 2
    for node in range(node_count):
        if node % NODES_PER_CHECKPOINT == 0:
            cp = HEADER.size + node // NODES_PER_CHECKPOINT * CHECKPOINT.size
            CHECKPOINT.pack_into(output, cp, offset, next_child, 0)
        if offset == len(data):
            raise ValueError("truncated NGTR record")
        label = data[offset]
        flags, offset = read_var_uint32(data, offset + 1)
        degree = flags >> 1
        if node >= next_child or degree > 255 or next_child + degree > node_count:
            raise ValueError(f"invalid child range at NGTR node {node}")
        if (label == 0) != (node < 2):
            raise ValueError(f"invalid label at NGTR node {node}")
        if flags & 1:
            terminal, offset = read_var_uint32(data, offset)
            if node < 2 or terminal == 0:
                raise ValueError(f"invalid terminal count at NGTR node {node}")
            masses[node] = terminal
        elif node >= 2 and degree == 0:
            raise ValueError(f"missing terminal count at NGTR leaf {node}")
        child_starts[node] = next_child
        child_counts[node] = degree
        labels[node] = label
        next_child += degree
    if next_child != node_count or offset != len(data):
        raise ValueError("NGTR file length does not match its records")

    for node in range(node_count - 1, -1, -1):
        if not child_counts[node]:
            continue
        start = child_starts[node]
        stop = start + child_counts[node]
        mass = masses[node]
        previous_label = 0
        for child in range(start, stop):
            if labels[child] <= previous_label:
                raise ValueError(f"invalid child order at NGTR node {node}")
            previous_label = labels[child]
            mass += masses[child]
        if mass > UINT32_MAX:
            raise ValueError(f"mass overflows at NGTR node {node}")
        masses[node] = mass

    mass_offset = Struct("<I")
    for node, degree in enumerate(child_counts):
        if node % NODES_PER_CHECKPOINT == 0:
            cp = HEADER.size + node // NODES_PER_CHECKPOINT * CHECKPOINT.size
            mass_offset.pack_into(output, cp + 8, len(output) - mass_start)
        if degree:
            append_var_uint32(output, masses[node])
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trie", type=Path, help="input NGTR v2, compact v3, or radix v4 model")
    parser.add_argument("index", type=Path, help="output NGIX index")
    args = parser.parse_args()
    try:
        if args.trie.resolve() == args.index.resolve() or (
            args.index.exists() and args.trie.samefile(args.index)
        ):
            raise ValueError("model and index must use different files")
        index = build_index(args.trie.read_bytes())
        args.index.write_bytes(index)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
