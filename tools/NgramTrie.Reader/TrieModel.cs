using System.Buffers.Binary;
using System.Text;

namespace NgramTrie.Reader;

public enum TrieRoot { Combined = 0, Unigrams = 1 }

public readonly record struct TrieNode(ulong Mass, int FirstChild, int ChildCount, byte Label,
    int ChildStride = 1, short LogProbability = 0, short LogBackoff = 0,
    short MaximumLogProbability = short.MinValue, byte ScoreFlags = 0)
{
    public int ChildAt(int ordinal) => (uint)ordinal < (uint)ChildCount
        ? unchecked((int)((uint)FirstChild + (uint)ordinal * (uint)ChildStride))
        : throw new ArgumentOutOfRangeException(nameof(ordinal));
}
public readonly record struct TrieTransition(byte Label, int NodeIndex, ulong Mass, double Probability);

/// <summary>A read-only NGTR v2/v3 or compressed radix v4 model. V4 node IDs are opaque sparse byte handles.</summary>
public sealed class TrieModel
{
    private const int HeaderSize = 16;
    private const int RecordSize = 12;
    private static readonly Encoding Utf8 = new UTF8Encoding(false, true);
    private readonly byte[] records;
    private readonly RadixModelData? radix;

    private TrieModel(byte[] records, Header header)
    {
        this.records = records;
        Version = header.Version;
        NodeCount = header.NodeCount;
        Validate();
    }

    private TrieModel(RadixModelData radix)
    {
        this.radix = radix; records = [];
        Version = 4; NodeCount = radix.LogicalNodeCount;
    }

    public uint Version { get; }
    public long NodeCount { get; }
    public int PhysicalNodeCount => radix?.PhysicalNodeCount ?? checked((int)NodeCount);
    public bool HasLanguageScores => radix?.HasLanguageScores ?? false;
    public long StorageBytes => radix?.StorageBytes ?? records.LongLength;

    public static TrieModel Load(string path)
    {
        using var source = File.OpenRead(path);
        var result = Load(source);
        string sidecar = Path.ChangeExtension(path, ".ngix");
        if (result.radix is not null && File.Exists(sidecar)) result.radix.VerifyIndex(File.ReadAllBytes(sidecar));
        return result;
    }

    /// <summary>Reads one complete model from the current stream position and leaves the stream open.</summary>
    public static TrieModel Load(Stream source)
    {
        ArgumentNullException.ThrowIfNull(source);
        try
        {
            Span<byte> header = stackalloc byte[HeaderSize];
            source.ReadExactly(header);
            var format = ParseHeader(header);

            if (format.Version == 4)
            {
                if (source.CanSeek)
                {
                    long length = source.Length - source.Position + HeaderSize;
                    if (length > Array.MaxLength) throw new InvalidDataException("Radix model exceeds the supported array size.");
                    var data = new byte[checked((int)length)]; header.CopyTo(data);
                    source.ReadExactly(data.AsSpan(HeaderSize));
                    return new(RadixModelData.Parse(data));
                }
                using var radixBuffer = new MemoryStream(); radixBuffer.Write(header);
                var radixChunk = new byte[65536]; int read;
                long maximum = Math.Min(Array.MaxLength, 16L + format.NodeCount * 274L);
                while ((read = source.Read(radixChunk)) != 0)
                {
                    if (radixBuffer.Length + read > maximum) throw new InvalidDataException("Radix model length exceeds its declared records.");
                    radixBuffer.Write(radixChunk, 0, read);
                }
                return new(RadixModelData.Parse(radixBuffer.ToArray()));
            }

            if (format.Version == 3)
            {
                bool lengthVerified = source.CanSeek;
                if (lengthVerified) CheckV3Length(source.Length - source.Position, format.NodeCount);
                return LoadV3(new RecordSource(source), format, lengthVerified);
            }

            int size = format.NodeCount * RecordSize;
            if (source.CanSeek)
            {
                if (source.Length - source.Position != size) throw new InvalidDataException("NGTR file length does not match its header.");
                var records = new byte[size];
                source.ReadExactly(records);
                return new(records, format);
            }

            // Grow on demand when the declared length cannot be checked against the stream up front.
            using var buffer = new MemoryStream();
            var chunk = new byte[Math.Min(size, 65536)];
            while (buffer.Length < size)
            {
                int count = (int)Math.Min(chunk.Length, size - buffer.Length);
                source.ReadExactly(chunk.AsSpan(0, count));
                buffer.Write(chunk, 0, count);
            }
            if (source.ReadByte() != -1) throw new InvalidDataException("Unexpected data after the NGTR records.");
            return new(buffer.ToArray(), format);
        }
        catch (EndOfStreamException error)
        {
            throw new InvalidDataException("Truncated NGTR file.", error);
        }
    }

    /// <summary>Loads and validates a copy of a complete binary model.</summary>
    public static TrieModel Parse(ReadOnlySpan<byte> data)
    {
        var format = ParseHeader(data);
        if (format.Version == 4) return new(RadixModelData.Parse(data.ToArray()));
        var content = data[HeaderSize..];
        if (format.Version == 3)
        {
            CheckV3Length(content.Length, format.NodeCount);
            try { return LoadV3(new RecordSource(content), format, lengthVerified: true); }
            catch (EndOfStreamException error) { throw new InvalidDataException("Truncated NGTR file.", error); }
        }

        if (content.Length != format.NodeCount * RecordSize)
            throw new InvalidDataException("NGTR file length does not match its header.");
        return new(content.ToArray(), format);
    }

    public TrieNode GetNode(int index)
    {
        if (radix is not null) return radix.GetNode(index);
        if ((uint)index >= (uint)NodeCount) throw new ArgumentOutOfRangeException(nameof(index));
        return ReadNode(index);
    }

    public int? FindChild(int nodeIndex, byte label) => radix is not null ? radix.FindChild(nodeIndex, label) : FindChild(GetNode(nodeIndex), label);

    private int? FindChild(TrieNode node, byte label)
    {
        int low = 0;
        int high = node.ChildCount - 1;
        while (low <= high)
        {
            int middle = low + (high - low) / 2;
            int handle = node.ChildAt(middle);
            byte candidate = radix is null ? records[handle * RecordSize + 10] : radix.GetNode(handle).Label;
            if (candidate == label) return handle;
            if (candidate < label) low = middle + 1;
            else high = middle - 1;
        }
        return null;
    }

    /// <summary>Finds an exact UTF-8 prefix. Pass tokens already normalized as in the corpus.</summary>
    public int? FindPrefix(string prefix, TrieRoot root = TrieRoot.Combined) => FindPrefix(Utf8.GetBytes(prefix), root);

    public int? FindPrefix(ReadOnlySpan<byte> prefix, TrieRoot root = TrieRoot.Combined)
    {
        if (root is not (TrieRoot.Combined or TrieRoot.Unigrams)) throw new ArgumentOutOfRangeException(nameof(root));
        return Follow((int)root, prefix);
    }

    /// <summary>Continues an exact UTF-8 lookup from a node already found in this model.</summary>
    public int? FollowFrom(int nodeIndex, ReadOnlySpan<byte> suffix)
    {
        if (radix is not null) return radix.FollowFrom(nodeIndex, suffix);
        if ((uint)nodeIndex >= (uint)NodeCount) throw new ArgumentOutOfRangeException(nameof(nodeIndex));
        return Follow(nodeIndex, suffix);
    }

    public IReadOnlyList<TrieTransition> GetTransitions(int nodeIndex)
    {
        var node = GetNode(nodeIndex);
        var transitions = new TrieTransition[node.ChildCount];
        for (int offset = 0; offset < transitions.Length; offset++)
        {
            int index = node.ChildAt(offset);
            var child = GetNode(index);
            transitions[offset] = new(child.Label, index, child.Mass, node.Mass == 0 ? 0 : (double)child.Mass / node.Mass);
        }
        return transitions;
    }

    public ulong GetTerminalCount(int nodeIndex)
    {
        if (radix is not null) return radix.GetTerminalCount(nodeIndex);
        var node = GetNode(nodeIndex);
        ulong count = node.Mass;
        for (int offset = 0; offset < node.ChildCount; offset++) count -= ReadNode(node.ChildAt(offset)).Mass;
        return count;
    }

    /// <summary>Gets the probability of ending the n-gram here, or null when the node has no mass.</summary>
    public double? GetTerminalProbability(int nodeIndex)
    {
        ulong mass = GetNode(nodeIndex).Mass;
        return mass == 0 ? null : (double)GetTerminalCount(nodeIndex) / mass;
    }

    /// <summary>Gets a descriptive whole-word count ratio from a root or context space. For smoothed conditional probabilities use ConditionalLogProbability.</summary>
    /// <returns>Zero for a missing word, or null when the starting node has no mass.</returns>
    public double? GetWordProbability(int wordStart, string word)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(word);
        if (word.Any(char.IsWhiteSpace)) throw new ArgumentException("Provide one word.", nameof(word));
        var start = GetNode(wordStart);
        if (wordStart is not (0 or 1) && start.Label != (byte)' ')
            throw new ArgumentException("The word must start at a root or after a space.", nameof(wordStart));
        if (start.Mass == 0) return null;
        if (Follow(wordStart, Utf8.GetBytes(word)) is not int end) return 0;

        ulong mass = GetTerminalCount(end);
        if (FindChild(end, (byte)' ') is int space) mass += GetNode(space).Mass;
        return (double)mass / start.Mass;
    }

    private int? Follow(int start, ReadOnlySpan<byte> prefix)
    {
        if (radix is not null) return radix.FollowFrom(start, prefix);
        foreach (byte label in prefix)
        {
            if (FindChild(ReadNode(start), label) is not int index) return null;
            start = index;
        }
        return start;
    }

    private static Header ParseHeader(ReadOnlySpan<byte> header)
    {
        if (header.Length < HeaderSize || !header[..4].SequenceEqual("NGTR"u8))
            throw new InvalidDataException("Invalid NGTR header.");
        uint version = BinaryPrimitives.ReadUInt32LittleEndian(header[4..]);
        if (version is not (2 or 3 or 4))
            throw new InvalidDataException("Only NGTR versions 2, 3 and 4 are supported.");
        uint count = BinaryPrimitives.ReadUInt32LittleEndian(header[8..]);
        if (version == 4)
        {
            uint logical = BinaryPrimitives.ReadUInt32LittleEndian(header[12..]);
            if (count < 2 || count > 0x00ffffff || logical < count || logical > 2 + (ulong)(count - 2) * 256)
                throw new InvalidDataException("Invalid radix edge or logical node count.");
            return new(version, (int)count);
        }
        uint recordSize = version == 2 ? (uint)RecordSize : 0u;
        if (count < 2 || (long)count * RecordSize > Array.MaxLength ||
            BinaryPrimitives.ReadUInt32LittleEndian(header[12..]) != recordSize)
            throw new InvalidDataException("Invalid NGTR node count or record size.");
        return new(version, (int)count);
    }

    private static void CheckV3Length(long size, int nodeCount)
    {
        // A label and flags take at least two bytes. Flags use at most two bytes
        // (one child per possible nonzero label), and a terminal count uses at most five.
        if (size < 2L * nodeCount || size > 8L * nodeCount)
            throw new InvalidDataException("NGTR file length does not match its header.");
    }

    private static TrieModel LoadV3(RecordSource source, Header header, bool lengthVerified)
    {
        // A header can claim far more nodes than an unverified stream provides.
        int size = header.NodeCount * RecordSize;
        var records = new byte[lengthVerified ? size : Math.Min(size, 256)];
        int nextChild = 2;
        for (int index = 0; index < header.NodeCount; index++)
        {
            byte label = source.ReadRequiredByte();
            uint flags = source.ReadVarUInt32();
            uint childCount = flags >> 1;
            if (index >= nextChild || childCount > byte.MaxValue || childCount > header.NodeCount - nextChild)
                throw new InvalidDataException($"Invalid child range for NGTR node {index}.");
            if (index < 2 ? label != 0 : label == 0)
                throw new InvalidDataException($"Invalid NGTR node {index}.");

            uint terminal = 0;
            if ((flags & 1) != 0)
            {
                terminal = source.ReadVarUInt32();
                if (terminal == 0 || index < 2)
                    throw new InvalidDataException($"Invalid terminal count for NGTR node {index}.");
            }
            if (index >= 2 && childCount == 0 && terminal == 0)
                throw new InvalidDataException($"Invalid NGTR leaf {index}.");

            int recordOffset = index * RecordSize;
            if (recordOffset > records.Length - RecordSize)
                Array.Resize(ref records, records.Length + Math.Min(records.Length, size - records.Length));

            Span<byte> record = records.AsSpan(recordOffset, RecordSize);
            BinaryPrimitives.WriteInt32LittleEndian(record, childCount == 0 ? 0 : nextChild);
            BinaryPrimitives.WriteUInt32LittleEndian(record[4..], terminal);
            BinaryPrimitives.WriteUInt16LittleEndian(record[8..], (ushort)childCount);
            record[10] = label;
            nextChild += (int)childCount;
        }
        if (nextChild != header.NodeCount || source.ReadByte() != -1)
            throw new InvalidDataException("NGTR file length does not match its header.");

        // Child ranges always follow their parent. Reconstruct masses in reverse BFS order.
        for (int index = header.NodeCount - 1; index >= 0; index--)
        {
            var raw = records.AsSpan(index * RecordSize, RecordSize);
            int firstChild = BinaryPrimitives.ReadInt32LittleEndian(raw);
            int childCount = BinaryPrimitives.ReadUInt16LittleEndian(raw[8..]);
            ulong mass = BinaryPrimitives.ReadUInt32LittleEndian(raw[4..]);
            for (int offset = 0; offset < childCount; offset++)
                mass += BinaryPrimitives.ReadUInt32LittleEndian(records.AsSpan((firstChild + offset) * RecordSize + 4, 4));
            if (mass > uint.MaxValue) throw new InvalidDataException($"NGTR mass overflows at node {index}.");
            BinaryPrimitives.WriteUInt32LittleEndian(raw[4..], (uint)mass);
        }
        return new(records, header);
    }

    private TrieNode ReadNode(int index)
    {
        var raw = records.AsSpan(index * RecordSize, RecordSize);
        return new(BinaryPrimitives.ReadUInt32LittleEndian(raw[4..]),
            unchecked((int)BinaryPrimitives.ReadUInt32LittleEndian(raw)),
            BinaryPrimitives.ReadUInt16LittleEndian(raw[8..]), raw[10]);
    }

    private void Validate()
    {
        int nextChild = 2;
        for (int index = 0; index < NodeCount; index++)
        {
            var node = ReadNode(index);
            if (index >= nextChild || records[index * RecordSize + 11] != 0 ||
                (index < 2 ? node.Label != 0 : node.Label == 0 || node.Mass == 0))
                throw new InvalidDataException($"Invalid NGTR node {index}.");
            if (node.ChildCount == 0 ? node.FirstChild != 0 :
                node.FirstChild != nextChild || node.ChildCount > NodeCount - nextChild)
                throw new InvalidDataException($"Invalid child range for NGTR node {index}.");

            ulong remainingMass = node.Mass;
            byte previousLabel = 0;
            for (int offset = 0; offset < node.ChildCount; offset++)
            {
                var child = ReadNode(nextChild + offset);
                if (child.Label <= previousLabel || child.Mass > remainingMass)
                    throw new InvalidDataException($"Invalid child order or mass for NGTR node {index}.");
                remainingMass -= child.Mass;
                previousLabel = child.Label;
            }
            if (index < 2 && remainingMass != 0)
                throw new InvalidDataException($"Unexpected terminal mass on NGTR root {index}.");
            nextChild += node.ChildCount;
        }
    }

    private readonly record struct Header(uint Version, int NodeCount);

    /// <summary>Gets a validated, canonical NGIX v2 sidecar for a compressed radix model.</summary>
    public byte[] CopyRadixIndex() => radix?.CopyIndex() ?? throw new InvalidOperationException("This is not a radix model.");

    public static TrieModel ParseRadix(ReadOnlySpan<byte> data, ReadOnlySpan<byte> lookupIndex)
    {
        var result = Parse(data);
        if (result.radix is null) throw new InvalidDataException("Expected an NGTR v4 radix model.");
        result.radix.VerifyIndex(lookupIndex);
        return result;
    }

    public static double DecodeLogScore(short value) => value == short.MinValue ? double.NegativeInfinity : value * (Math.Log(2) / 256);

    /// <summary>Calibrated natural-log word probability for a scored radix model, including missing-context backoff.</summary>
    public double ConditionalLogProbability(string word, string previous = "", string older = "") =>
        ConditionalLogProbability(Utf8.GetBytes(word), Utf8.GetBytes(previous), Utf8.GetBytes(older));

    public double ConditionalLogProbability(ReadOnlySpan<byte> word, ReadOnlySpan<byte> previous, ReadOnlySpan<byte> older)
    {
        if (!HasLanguageScores) return double.NegativeInfinity;
        int? unigram = FindPrefix(word, TrieRoot.Unigrams);
        var item = GetNode(unigram ?? (int)TrieRoot.Unigrams);
        if ((item.ScoreFlags & 2) == 0) item = GetNode((int)TrieRoot.Unigrams);
        double result = DecodeLogScore(item.LogProbability);
        if (!previous.IsEmpty)
        {
            int? history = FindPrefix(previous);
            result = Apply(history, word, result);
            if (!older.IsEmpty)
            {
                int? first = FindPrefix(older);
                int? space = first is int start ? FindChild(start, (byte)' ') : null;
                history = space is int after ? FollowFrom(after, previous) : null;
                result = Apply(history, word, result);
            }
        }
        return result;

        double Apply(int? history, ReadOnlySpan<byte> target, double lower)
        {
            if (history is not int start) return lower;
            var context = GetNode(start);
            int? space = FindChild(start, (byte)' ');
            int? observed = space is int after ? FollowFrom(after, target) : null;
            if (observed is int node)
            {
                var value = GetNode(node);
                if ((value.ScoreFlags & 2) != 0) return DecodeLogScore(value.LogProbability);
            }
            return (context.ScoreFlags & 4) != 0 ? DecodeLogScore(context.LogBackoff) + lower : lower;
        }
    }

    private ref struct RecordSource
    {
        private readonly Stream? stream;
        private ReadOnlySpan<byte> remaining;

        public RecordSource(Stream stream)
        {
            this.stream = stream;
            remaining = default;
        }

        public RecordSource(ReadOnlySpan<byte> bytes)
        {
            stream = null;
            remaining = bytes;
        }

        public int ReadByte()
        {
            if (stream is not null) return stream.ReadByte();
            if (remaining.IsEmpty) return -1;
            byte value = remaining[0];
            remaining = remaining[1..];
            return value;
        }

        public byte ReadRequiredByte()
        {
            int value = ReadByte();
            if (value < 0) throw new EndOfStreamException();
            return (byte)value;
        }

        public uint ReadVarUInt32()
        {
            uint value = 0;
            for (int shift = 0; shift <= 28; shift += 7)
            {
                byte next = ReadRequiredByte();
                if (shift == 28 && (next & 0xf0) != 0)
                    throw new InvalidDataException("NGTR variable-length integer exceeds 32 bits.");

                value |= (uint)(next & 0x7f) << shift;
                if ((next & 0x80) != 0) continue;
                if (shift != 0 && next == 0)
                    throw new InvalidDataException("NGTR variable-length integer is not canonical.");
                return value;
            }
            throw new InvalidDataException("Invalid NGTR variable-length integer.");
        }
    }
}
