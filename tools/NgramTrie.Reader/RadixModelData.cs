using System.Buffers.Binary;

namespace NgramTrie.Reader;

// Owns compact model/index byte arrays. Label spans stay compressed; byte
// handles expose positions inside them without allocating a byte-node graph.
internal sealed class RadixModelData
{
    private const int Stride = 256, CheckpointSize = 12;
    private readonly byte[] model;
    private readonly byte[] index;
    private readonly int massStart;
    public int PhysicalNodeCount { get; }
    public long LogicalNodeCount { get; }
    public bool HasLanguageScores { get; }
    public long StorageBytes => model.LongLength + index.LongLength;

    private RadixModelData(byte[] model, byte[] index, int physical, uint logical, bool scored)
    {
        this.model = model; this.index = index;
        PhysicalNodeCount = physical; LogicalNodeCount = logical; HasLanguageScores = scored;
        massStart = 16 + ((physical + 31) / 32) * CheckpointSize;
    }
    private readonly record struct Record(int Id, int Span, int Length, int Children, int FirstChild,
        ulong Terminal, short Probability, short Backoff, byte Flags,
        ulong Mass = 0, short Maximum = short.MinValue);
    private struct Cursor { public int Id, Offset, NextChild, MassOffset; }

    public static RadixModelData Parse(byte[] owned)
    {
        if (owned.Length < 20 || !owned.AsSpan(0, 4).SequenceEqual("NGTR"u8) || U32(owned, 4) != 4)
            throw new InvalidDataException("Invalid radix-v4 header.");
        uint count = U32(owned, 8), logical = U32(owned, 12);
        if (count < 2 || count > 0x00ffffff || count > (owned.Length - 16) / 2 ||
            logical < count || logical > 2 + (ulong)(count - 2) * 256)
            throw new InvalidDataException("Invalid radix edge or logical-node count.");
        int physical = (int)count;
        var offsets = new int[physical];
        var first = new int[physical];
        var masses = new ulong[physical];
        var bounds = new short[physical];
        int offset = 16, nextChild = 2;
        ulong expanded = 0; bool scored = false;
        for (int id = 0; id < physical; id++)
        {
            if (id >= nextChild) throw new InvalidDataException("Radix model has unreachable edges.");
            offsets[id] = offset; first[id] = nextChild;
            var record = ReadRecord(owned, id, ref offset, nextChild);
            if (record.Children > physical - nextChild) throw new InvalidDataException("Invalid radix child range.");
            nextChild += record.Children;
            expanded += id < 2 ? 1u : (uint)record.Length;
            if (id == 0)
            {
                scored = (record.Flags & 2) != 0;
                if ((record.Flags & 4) != 0 || (scored && record.Probability != 0))
                    throw new InvalidDataException("Invalid scored-model root tag.");
            }
            else if (id == 1)
            {
                if ((record.Flags & 4) != 0 || ((record.Flags & 2) != 0) != scored)
                    throw new InvalidDataException("Missing or invalid unknown-word probability.");
            }
            else if ((record.Flags != 0 && !scored) || (scored && record.Terminal != 0 && (record.Flags & 2) == 0))
                throw new InvalidDataException("Inconsistent radix language-score metadata.");
            masses[id] = record.Terminal;
            bounds[id] = id >= 2 && (record.Flags & 2) != 0 ? record.Probability : short.MinValue;
        }
        if (offset != owned.Length || nextChild != physical || expanded != logical)
            throw new InvalidDataException("Radix length or logical-node count does not match its records.");
        // Parents precede their physical children, so both exact masses and
        // maximum-score bounds can be computed in a reverse pass.
        for (int id = physical - 1; id >= 0; id--)
        {
            int position = offsets[id];
            var record = ReadRecord(owned, id, ref position, first[id]);
            byte previous = 0;
            for (int ordinal = 0; ordinal < record.Children; ordinal++)
            {
                int child = first[id] + ordinal, childOffset = offsets[child];
                uint length = Var32(owned, ref childOffset);
                if (length == 0 || owned[childOffset] <= previous)
                    throw new InvalidDataException("Radix children are not strictly byte ordered.");
                previous = owned[childOffset];
                try { masses[id] = checked(masses[id] + masses[child]); }
                catch (OverflowException error) { throw new InvalidDataException("Radix subtree count exceeds 64 bits.", error); }
                bounds[id] = Math.Max(bounds[id], bounds[child]);
            }
        }
        int checkpoints = (physical + 31) / 32;
        var checkpointBytes = new byte[checkpoints * CheckpointSize];
        using var massBytes = new MemoryStream();
        for (int id = 0; id < physical; id++)
        {
            if ((id & 31) == 0)
            {
                var checkpoint = checkpointBytes.AsSpan((id / 32) * CheckpointSize, CheckpointSize);
                BinaryPrimitives.WriteUInt32LittleEndian(checkpoint, (uint)offsets[id]);
                BinaryPrimitives.WriteUInt32LittleEndian(checkpoint[4..], (uint)first[id]);
                BinaryPrimitives.WriteUInt32LittleEndian(checkpoint[8..], checked((uint)massBytes.Length));
            }
            int position = offsets[id];
            var record = ReadRecord(owned, id, ref position, first[id]);
            if (record.Children == 0) continue;
            WriteVar(massBytes, masses[id]);
            if (scored) { massBytes.WriteByte((byte)bounds[id]); massBytes.WriteByte((byte)(bounds[id] >> 8)); }
        }
        long size = 16L + checkpointBytes.Length + massBytes.Length;
        if (size > Array.MaxLength) throw new InvalidDataException("Radix index exceeds the supported array size.");
        var index = new byte[(int)size];
        "NGIX"u8.CopyTo(index);
        BinaryPrimitives.WriteUInt32LittleEndian(index.AsSpan(4), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(index.AsSpan(8), (uint)owned.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(index.AsSpan(12), count);
        checkpointBytes.CopyTo(index, 16);
        if (!massBytes.TryGetBuffer(out var segment)) throw new InvalidDataException("Cannot access radix index buffer.");
        segment.AsSpan(0, (int)massBytes.Length).CopyTo(index.AsSpan(16 + checkpointBytes.Length));
        return new(owned, index, physical, logical, scored);
    }

    public void VerifyIndex(ReadOnlySpan<byte> supplied)
    {
        if (!supplied.SequenceEqual(index)) throw new InvalidDataException("The radix lookup index is stale or invalid for this model.");
    }
    public byte[] CopyIndex() => (byte[])index.Clone();

    private static uint U32(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes.Slice(offset, 4));
    private static uint Var32(ReadOnlySpan<byte> bytes, ref int offset)
    {
        ulong value = Var64(bytes, ref offset);
        return value <= uint.MaxValue ? (uint)value : throw new InvalidDataException("Radix integer exceeds 32 bits.");
    }
    private static ulong Var64(ReadOnlySpan<byte> bytes, ref int offset)
    {
        ulong value = 0;
        for (int shift = 0; shift <= 63; shift += 7)
        {
            if ((uint)offset >= (uint)bytes.Length) throw new InvalidDataException("Truncated radix integer.");
            byte next = bytes[offset++];
            if (shift == 63 && next > 1) throw new InvalidDataException("Radix integer exceeds 64 bits.");
            value |= (ulong)(next & 127) << shift;
            if ((next & 128) != 0) continue;
            if (shift != 0 && next == 0) throw new InvalidDataException("Radix integer is not canonical.");
            return value;
        }
        throw new InvalidDataException("Invalid radix integer.");
    }
    private static short I16(ReadOnlySpan<byte> bytes, ref int offset)
    {
        if (offset > bytes.Length - 2) throw new InvalidDataException("Truncated radix score.");
        short value = BinaryPrimitives.ReadInt16LittleEndian(bytes.Slice(offset, 2)); offset += 2; return value;
    }
    private static void WriteVar(Stream stream, ulong value)
    {
        while (value >= 128) { stream.WriteByte((byte)((value & 127) | 128)); value >>= 7; }
        stream.WriteByte((byte)value);
    }
    private static Record ReadRecord(ReadOnlySpan<byte> data, int id, ref int offset, int nextChild)
    {
        uint length = Var32(data, ref offset);
        if (length > 256 || (length == 0) != (id < 2) || length > data.Length - offset)
            throw new InvalidDataException("Invalid radix label span.");
        int span = offset;
        for (int i = 0; i < length; i++) if (data[offset + i] == 0) throw new InvalidDataException("Radix label contains zero.");
        offset += (int)length;
        uint flags = Var32(data, ref offset);
        int children = (int)(flags >> 3);
        if (children > 255) throw new InvalidDataException("Invalid radix child count.");
        ulong terminal = (flags & 1) != 0 ? Var64(data, ref offset) : 0;
        if ((flags & 1) != 0 && (id < 2 || terminal == 0)) throw new InvalidDataException("Invalid radix terminal count.");
        short probability = (flags & 2) != 0 ? I16(data, ref offset) : (short)0;
        short backoff = (flags & 4) != 0 ? I16(data, ref offset) : (short)0;
        if ((flags & 2) != 0 && probability > 0) throw new InvalidDataException("Radix probability exceeds one.");
        if (id >= 2 && children == 0 && terminal == 0 && (flags & 6) == 0)
            throw new InvalidDataException("Radix leaf has no terminal or score metadata.");
        return new(id, span, (int)length, children, nextChild, terminal, probability, backoff, (byte)(flags & 6));
    }
    private Cursor At(int physical)
    {
        int checkpoint = 16 + (physical / 32) * CheckpointSize;
        var cursor = new Cursor { Id = physical & ~31, Offset = (int)U32(index, checkpoint),
            NextChild = (int)U32(index, checkpoint + 4), MassOffset = massStart + (int)U32(index, checkpoint + 8) };
        while (cursor.Id < physical) Next(ref cursor);
        return cursor;
    }
    private Record Next(ref Cursor cursor)
    {
        var record = ReadRecord(model, cursor.Id, ref cursor.Offset, cursor.NextChild);
        cursor.Id++; cursor.NextChild += record.Children;
        ulong mass = record.Children == 0 ? record.Terminal : Var64(index, ref cursor.MassOffset);
        short maximum = record.Id >= 2 && (record.Flags & 2) != 0 ? record.Probability : short.MinValue;
        if (HasLanguageScores && record.Children > 0) maximum = I16(index, ref cursor.MassOffset);
        return record with { Mass = mass, Maximum = maximum };
    }
    private Record Physical(int physical)
    {
        if ((uint)physical >= (uint)PhysicalNodeCount) throw new ArgumentOutOfRangeException(nameof(physical));
        var cursor = At(physical); return Next(ref cursor);
    }
    private static int Edge(int handle) => handle < 2 && handle >= 0 ? handle : checked((int)(2 + (unchecked((uint)handle) - 2) / Stride));
    private static int Position(int handle) => handle < 2 && handle >= 0 ? 0 : (int)((unchecked((uint)handle) - 2) % Stride);
    private static int Handle(int physical, int position = 0) => physical < 2 ? physical :
        unchecked((int)(2u + (uint)(physical - 2) * Stride + (uint)position));
    private Record Located(int handle, out int position)
    {
        int edge = Edge(handle); position = Position(handle);
        var record = Physical(edge);
        if (edge >= 2 && position >= record.Length) throw new ArgumentOutOfRangeException(nameof(handle));
        return record;
    }
    public TrieNode GetNode(int handle)
    {
        var record = Located(handle, out int position);
        byte label = record.Id < 2 ? (byte)0 : model[record.Span + position];
        if (record.Id >= 2 && position + 1 < record.Length)
            return new(record.Mass, unchecked(handle + 1), 1, label, 1, 0, 0, record.Maximum, 0);
        return new(record.Mass, record.Children == 0 ? 0 : Handle(record.FirstChild), record.Children,
            label, Stride, record.Probability, record.Backoff, record.Maximum, record.Flags);
    }
    public ulong GetTerminalCount(int handle)
    {
        var record = Located(handle, out int position);
        return record.Id < 2 || position + 1 < record.Length ? 0 : record.Terminal;
    }
    private int? Child(Record parent, int position, byte label, out Record child)
    {
        child = default;
        if (label == 0) return null;
        if (parent.Id >= 2 && position + 1 < parent.Length)
        {
            if (model[parent.Span + position + 1] != label) return null;
            child = parent; return Handle(parent.Id, position + 1);
        }
        if (parent.Children == 0) return null;
        var cursor = At(parent.FirstChild);
        for (int i = 0; i < parent.Children; i++)
        {
            var candidate = Next(ref cursor);
            byte first = model[candidate.Span];
            if (first > label) break;
            if (first == label) { child = candidate; return Handle(candidate.Id); }
        }
        return null;
    }
    public int? FindChild(int handle, byte label)
    {
        var record = Located(handle, out int position);
        return Child(record, position, label, out _);
    }
    public int? FollowFrom(int handle, ReadOnlySpan<byte> suffix)
    {
        var current = Located(handle, out int position);
        while (!suffix.IsEmpty)
        {
            if (current.Id >= 2 && position + 1 < current.Length)
            {
                int count = Math.Min(suffix.Length, current.Length - position - 1);
                if (!model.AsSpan(current.Span + position + 1, count).SequenceEqual(suffix[..count])) return null;
                position += count; handle = Handle(current.Id, position); suffix = suffix[count..];
                if (suffix.IsEmpty) return handle;
            }
            int? found = Child(current, position, suffix[0], out var child);
            if (found is not int next) return null;
            current = child; handle = next; position = Position(next); suffix = suffix[1..];
        }
        return handle;
    }
}
