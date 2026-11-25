using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using KeyboardSimulator;
using NgramTrie.Reader;

namespace KeyboardLanguageChecks;

// Reader/scoring expectations are independent of the native encoder. Optional
// native runs additionally verify production encoder/index interoperability.
internal static class RadixChecks
{
    private static void Check(bool value, string message) { if (!value) throw new InvalidOperationException(message); }
    private sealed class Node
    {
        public SortedDictionary<byte, Node> Children = [];
        public ulong Count;
        public double? Probability, Backoff;
        public byte[] Span = [];
    }
    private static void Put(Node root, string key, ulong count, double? probability = null, double? backoff = null)
    {
        foreach (byte label in Encoding.UTF8.GetBytes(key))
        {
            if (!root.Children.TryGetValue(label, out var child)) root.Children[label] = child = new();
            root = child;
        }
        root.Count = count; root.Probability = probability; root.Backoff = backoff;
    }
    private static short Quantize(double probability) => probability == 0 ? short.MinValue :
        (short)Math.Clamp(Math.Round(Math.Log2(probability) * 256, MidpointRounding.AwayFromZero), short.MinValue + 1, short.MaxValue);
    private static void Var(Stream stream, ulong value)
    {
        while (value >= 128) { stream.WriteByte((byte)((value & 127) | 128)); value >>= 7; }
        stream.WriteByte((byte)value);
    }
    private static byte[] Encode(Node combined, Node unigrams)
    {
        var physical = new List<Node> { combined, unigrams };
        uint logical = 2;
        for (int id = 0; id < physical.Count; id++)
            foreach (var child in physical[id].Children)
            {
                var span = new List<byte> { child.Key }; var end = child.Value;
                while (span.Count < 256 && end.Count == 0 && end.Probability is null && end.Backoff is null && end.Children.Count == 1)
                { var next = end.Children.First(); span.Add(next.Key); end = next.Value; }
                var edge = new Node { Count = end.Count, Probability = end.Probability, Backoff = end.Backoff,
                    Children = end.Children, Span = span.ToArray() };
                logical += (uint)span.Count; physical.Add(edge);
            }
        using var output = new MemoryStream(); using var writer = new BinaryWriter(output, Encoding.UTF8, leaveOpen: true);
        output.Write("NGTR"u8); writer.Write(4u); writer.Write((uint)physical.Count); writer.Write(logical);
        foreach (var node in physical)
        {
            Var(output, (uint)node.Span.Length); output.Write(node.Span);
            Var(output, (uint)(node.Children.Count * 8 + (node.Count != 0 ? 1 : 0) +
                (node.Probability.HasValue ? 2 : 0) + (node.Backoff.HasValue ? 4 : 0)));
            if (node.Count != 0) Var(output, node.Count);
            if (node.Probability.HasValue) writer.Write(Quantize(node.Probability.Value));
            if (node.Backoff.HasValue) writer.Write(Quantize(node.Backoff.Value));
        }
        return output.ToArray();
    }
    private static void Reject(byte[] raw, string reason)
    {
        try { TrieModel.Parse(raw); } catch (InvalidDataException) { return; }
        throw new InvalidOperationException("Reader accepted " + reason);
    }
    private sealed class NonSeekStream(byte[] bytes) : MemoryStream(bytes)
    {
        public override bool CanSeek => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    }
    public static void Run(string profiles, string? native, string? policyProbe = null)
    {
        var combined = new Node(); var unigrams = new Node();
        string longWord = new('a', 255); longWord += "é" + new string('b', 55);
        var words = new Dictionary<string, ulong>(StringComparer.Ordinal) {
            ["alphabet"] = 1UL << 40, ["alphabetical"] = 9, ["alpine"] = 5,
            ["zebra"] = 2, ["你好"] = 7, [longWord] = 3
        };
        foreach (var pair in words) { Put(combined, pair.Key, pair.Value); Put(unigrams, pair.Key, pair.Value); }
        Put(combined, "alphabet zebra", 4);
        byte[] raw = Encode(combined, unigrams);
        var countModel = TrieModel.Parse(raw);
        Check(countModel.Version == 4 && !countModel.HasLanguageScores && countModel.PhysicalNodeCount < countModel.NodeCount / 4,
            "Radix spans expanded into physical byte nodes");
        Check(countModel.StorageBytes < countModel.NodeCount * 12, "Radix reader retained a dense byte-node graph");
        foreach (var word in words)
        {
            int end = countModel.FindPrefix(word.Key, TrieRoot.Unigrams) ?? throw new InvalidOperationException("Missing radix word " + word.Key);
            Check(countModel.GetTerminalCount(end) == word.Value, "Exact 64-bit terminal count changed");
            foreach (int length in Enumerable.Range(0, word.Key.Length + 1))
            {
                if (length > 0 && char.IsHighSurrogate(word.Key[length - 1])) continue;
                Check(countModel.FindPrefix(word.Key[..length], TrieRoot.Unigrams) is not null, "Interior span prefix was lost");
            }
        }
        int alphabet = countModel.FindPrefix("alphabet", TrieRoot.Unigrams)!.Value;
        Check(countModel.GetNode(alphabet).ChildStride == 256, "Physical branches did not expose opaque child handles");
        Check(countModel.FindPrefix("alphaz", TrieRoot.Unigrams) is null && countModel.GetTerminalCount(countModel.FindPrefix("alpha", TrieRoot.Unigrams)!.Value) == 0,
            "Interior span was treated as an exact word");
        Check(countModel.GetTransitions(alphabet).Single().Label == (byte)'i' &&
            countModel.GetWordProbability(1, "alphabet") > .99, "Transition or count-ratio APIs assumed dense node handles");
        Check(TrieEntries.Read(countModel, default)[0].Single(entry => entry.Text == "alphabet").Count == 1UL << 40,
            "Viewer truncated a terminal count to 32 bits");
        var index = countModel.CopyRadixIndex();
        Check(TrieModel.ParseRadix(raw, index).GetNode(1).Mass == countModel.GetNode(1).Mass, "Indexed model changed subtree mass");
        using (var source = new NonSeekStream(raw)) Check(TrieModel.Load(source).NodeCount == countModel.NodeCount, "Nonseek radix load failed");
        index[^1] ^= 1;
        try { TrieModel.ParseRadix(raw, index); throw new InvalidOperationException("Reader accepted stale NGIX metadata"); }
        catch (InvalidDataException) { }
        Reject(raw[..^1], "a truncated radix record");
        Reject([.. raw, 0], "radix trailing bytes");
        var logical = (byte[])raw.Clone(); BinaryPrimitives.WriteUInt32LittleEndian(logical.AsSpan(12), 2); Reject(logical, "incorrect logical-node count");
        var nonminimal = new byte[raw.Length + 1]; raw.AsSpan(0, 16).CopyTo(nonminimal); nonminimal[16] = 128; nonminimal[17] = 0; raw.AsSpan(17).CopyTo(nonminimal.AsSpan(18));
        Reject(nonminimal, "a noncanonical radix varint");
        byte[] wrongOrder = [.. "NGTR"u8, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0,
            0, 16, 0, 0, 1, (byte)'z', 1, 1, 1, (byte)'a', 1, 1];
        Reject(wrongOrder, "unordered physical children");
        var overflowing = new Node(); Put(overflowing, "a", ulong.MaxValue); Put(overflowing, "b", 1);
        Reject(Encode(overflowing, new()), "64-bit subtree-mass overflow");

        var scored = ScoredFixture(); var model = TrieModel.Parse(scored);
        Check(model.HasLanguageScores, "Scored radix tag was ignored");
        double unknown = Math.Exp(model.ConditionalLogProbability("missing"));
        Check(Math.Abs(unknown - .01) < .0001 && Math.Abs(Math.Exp(model.ConditionalLogProbability("alpha", "missing", "missing")) - .4) < .001,
            "Unknown bucket or unseen-context backoff changed");
        var vocabulary = new[] { "alpha", "beta", "gamma", "one", "two", "delta" };
        foreach (var (previous, older) in new[] { ("", ""), ("one", ""), ("one", "two"), ("missing", "two") })
        {
            double total = vocabulary.Append("missing").Sum(word => Math.Exp(model.ConditionalLogProbability(word, previous, older)));
            Check(Math.Abs(total - 1) < .004, "Quantized interpolation lost probability normalization");
            foreach (string prefix in new[] { "", "a", "b", "g", "on", "zz" })
            {
                var expected = vocabulary.Where(word => word.StartsWith(prefix, StringComparison.Ordinal))
                    .OrderByDescending(word => model.ConditionalLogProbability(word, previous, older))
                    .ThenBy(word => Encoding.UTF8.GetBytes(word), ByteOrder.Instance).Take(3).ToArray();
                string context = older.Length == 0 ? previous + " " : older + " " + previous + " ";
                var actual = TrieSuggestions.Predict(model, context, prefix, default, language: "en")
                    .Select(word => word.ToLowerInvariant()).ToArray();
                Check(actual.SequenceEqual(expected), $"Bounded scored suggestions differ from exhaustive scoring for '{context}|{prefix}': {string.Join(',', actual)} != {string.Join(',', expected)}");
            }
        }
        // An observed trigram with a low probability must be excluded from all
        // lower-order layers, even if it did not enter the top-three list.
        var excluded = TrieModel.Parse(ScoredFixture(lowTrigram: true));
        var excludedResult = TrieSuggestions.Predict(excluded, "two one ", "", default, language: "en").Select(word => word.ToLowerInvariant()).ToArray();
        Check(!excludedResult.Contains("alpha"), "A low-scored observed trigram leaked through lower-order backoff");
        var keyboard = new KeyboardNeighborMap(KeyboardDefinition.Load(Path.Combine(profiles, "en-US-qwerty.json")));
        var correction = TrieCorrections.Find(model, keyboard, "alpa", "one", "two", 3, word => word, [], default);
        Check(correction.Contains("alpha"), "Radix Unicode edit search did not recover a deletion");
        Check(TrieCorrections.Find(model, keyboard, "alpha", "one", "two", 3, word => word, [], default).Count == 0,
            "An exact radix spelling was offered as an edit correction");
        var zero = TrieModel.Parse(ZeroProbabilityFixture());
        Check(double.IsNegativeInfinity(zero.ConditionalLogProbability("car", "one")) &&
            double.IsNegativeInfinity(zero.ConditionalLogProbability("cat", "one")), "Exact-zero observed probabilities became backoff probabilities");
        var zeroSuggestions = TrieCorrections.Find(zero, keyboard, "caz", "one", "", 3, word => word, [], default);
        Check(!zeroSuggestions.Contains("car") && !zeroSuggestions.Contains("cat"),
            "Zero-probability contextual words were offered through the edit/frequency fallback");
        var zeroCorrection = TrieCorrections.BestAutocorrection(zero, keyboard, "cat", "one", "", default);
        Check(zeroCorrection is { } acceptedZero && acceptedZero.Word == "cot" && double.IsFinite(acceptedZero.Probability) &&
            acceptedZero.Probability > 0 && acceptedZero.Probability <= 1,
            "A zero-probability original/candidate poisoned correction confidence or retained the forbidden candidate");
        SafetyPolicy(keyboard, policyProbe);
        if (native is not null) Native(native, profiles, keyboard);
        Console.WriteLine("Radix v4 spans, exact 64-bit counts, canonical index, malformed input, interpolated scoring, bounded top-three and autocorrect checks passed.");
    }
    private static void SafetyPolicy(KeyboardNeighborMap keyboard, string? nativeProbe)
    {
        var cases = new (string Name, (string, ulong)[] Words, string Typed, string Expected)[] {
            ("known-retention", [("car",1000000000000UL),("cat",1)], "cat", ""),
            ("deletion", [("garden",10000000000UL)], "garde", "garden"),
            ("transposition", [("garden",10000000000UL)], "gadren", "garden"),
            ("unicode-deletion", [("école",10000000000UL)], "écle", "école"),
            ("supplementary-deletion", [("𠀀cole",10000000000UL)], "𠀀cle", "𠀀cole"),
            ("multiple-edits", [("garden",1000000000000UL)], "gdn", ""),
            ("keep-margin", [("garden",3900)], "garde", ""),
            ("ambiguous-runner", [("café",10000000000UL),("cafà",10000000000UL)], "cafz", "")
        };
        var outcomes = new Dictionary<string, (string Word, double Probability)>(StringComparer.Ordinal);
        foreach (var item in cases)
        {
            var combined = new Node(); var unigrams = new Node();
            foreach (var (word, count) in item.Words) { Put(combined,word,count); Put(unigrams,word,count); }
            var model = TrieModel.Parse(Encode(combined,unigrams));
            var result = TrieCorrections.BestAutocorrection(model,keyboard,item.Typed,"","",default);
            Check((result?.Word ?? "") == item.Expected && (!result.HasValue || double.IsFinite(result.Value.Probability)),
                "Autocorrect safety policy regression: " + item.Name);
            outcomes.Add(item.Name,(result?.Word ?? "",result?.Probability ?? 0));
            if (item.Name == "known-retention")
                Check(TrieSuggestions.Predict(model,"","ca",default,language:"en").Select(word => word.ToLowerInvariant()).SequenceEqual(["car","cat"]),
                    "Automatic safety gate changed manual suggestion ranking");
        }
        if (nativeProbe is not null)
        {
            string work = Path.Combine(Path.GetTempPath(),"ngtr-policy-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(work);
            try
            {
                var start = new ProcessStartInfo(nativeProbe) { UseShellExecute=false,RedirectStandardOutput=true,RedirectStandardError=true,CreateNoWindow=true };
                start.ArgumentList.Add(work);
                using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start native policy probe.");
                var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(30000)) { process.Kill(entireProcessTree:true); throw new TimeoutException("Native policy fixtures timed out."); }
                Task.WaitAll(stdout,stderr); Check(process.ExitCode == 0,"Native policy probe failed: " + stdout.Result + stderr.Result);
                int checkedRows = 0;
                foreach (string line in File.ReadLines(Path.Combine(work,"autocorrect-policy.tsv")))
                {
                    var fields = line.Split('\t'); var expected = outcomes[fields[0]];
                    Check((fields[1] == "1") == (expected.Word.Length > 0) && fields[2] == expected.Word &&
                        Math.Abs(double.Parse(fields[3],CultureInfo.InvariantCulture) - expected.Probability) < 1e-12,
                        "Native/managed correction acceptance or confidence differs: " + fields[0]);
                    checkedRows++;
                }
                Check(checkedRows == outcomes.Count,"Native correction parity omitted a fixture");
            }
            finally { Directory.Delete(work,recursive:true); }
        }
        Console.WriteLine("Correction safety fixtures passed" + (nativeProbe is null ? "." : " with exact native/managed decision and confidence parity."));
    }
    private sealed class ByteOrder : IComparer<byte[]>
    {
        public static readonly ByteOrder Instance = new();
        public int Compare(byte[]? left, byte[]? right) => left.AsSpan().SequenceCompareTo(right);
    }
    private static byte[] ScoredFixture(bool lowTrigram = false)
    {
        var combined = new Node { Probability = 1 }; var unigrams = new Node { Probability = .01 };
        foreach (var (word, probability) in new[] { ("alpha", .4), ("beta", .25), ("gamma", .15), ("one", .09), ("two", .06), ("delta", .04) })
        { Put(combined, word, 100, probability); Put(unigrams, word, 100, probability); }
        Put(combined, "one", 100, .09, .5);
        Put(combined, "one alpha", 30, .7);
        Put(combined, "two one", 0, null, lowTrigram ? .99 / .3 : .4 / .875);
        Put(combined, lowTrigram ? "two one alpha" : "two one beta", 1, lowTrigram ? .01 : .6);
        return Encode(combined, unigrams);
    }
    private static byte[] ZeroProbabilityFixture()
    {
        var combined = new Node { Probability = 1 }; var unigrams = new Node { Probability = .01 };
        foreach (var (word, count, probability) in new[] {
            ("car", 100UL, .3), ("cat", 100UL, .3), ("cot", 1000000UL, .3), ("one", 1UL, .09) })
        { Put(combined, word, count, probability); Put(unigrams, word, count, probability); }
        Put(combined, "one", 1, .09, 2);
        Put(combined, "one car", 1, 0); Put(combined, "one cat", 1, 0); Put(combined, "one cot", 1, .8);
        // P(car|one)=P(cat|one)=0; P(cot|one)=.8; the remaining
        // lower-order mass (.1) receives backoff 2, totaling one.
        return Encode(combined, unigrams);
    }
    private static void Native(string native, string profiles, KeyboardNeighborMap keyboard)
    {
        string work = Path.Combine(Path.GetTempPath(), "ngtr-managed-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        try
        {
            string input = Path.Combine(work, "corpus.txt");
            File.WriteAllText(input, string.Concat(Enumerable.Repeat("the quick brown fox. the quick blue bird. a slow brown fox. a quick brown bird. ", 20)) +
                "the quick green frog. the slow blue fox. a slow blue bird. alpha alphabet alphabetical alpine zebra.", new UTF8Encoding(false));
            var models = new List<TrieModel>();
            foreach (var (format, smoothing) in new[] { ("legacy", "counts"), ("compact", "counts"), ("radix", "counts"), ("radix", "modified-kn") })
            {
                string output = Path.Combine(work, format + "-" + smoothing + ".ngtr");
                var start = new ProcessStartInfo(native) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
                foreach (string arg in new[] { "--trie-output", output, "--language", "en", "--no-wordlist", "--format", format,
                    "--smoothing", smoothing, "--memory-mib", "32", "--workers", "1", "--no-cache", "--", input }) start.ArgumentList.Add(arg);
                using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start native generator.");
                Task<string> stdout = process.StandardOutput.ReadToEndAsync(), stderr = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(60000)) { process.Kill(entireProcessTree: true); throw new TimeoutException("Small native fixture generation timed out."); }
                Task.WaitAll(stdout, stderr);
                Check(process.ExitCode == 0, "Native fixture failed: " + stdout.Result + stderr.Result);
                var generated = TrieModel.Load(output); models.Add(generated);
                Check(generated.HasLanguageScores == (smoothing == "modified-kn"), "Fresh native scoring metadata was ignored");
                if (format == "radix")
                    Check(generated.CopyRadixIndex().SequenceEqual(File.ReadAllBytes(Path.ChangeExtension(output, ".ngix"))),
                        "Managed canonical index differs byte-for-byte from the fresh native index");
            }
            var baseline = TrieEntries.Read(models[0], default);
            foreach (var model in models.Skip(1))
                Check(TrieEntries.Read(model, default).SelectMany(group => group).SequenceEqual(baseline.SelectMany(group => group)),
                    "Fresh v2/v3/v4 models disagree on n-gram spellings or exact counts");
            var vocabulary = baseline[0].Select(entry => entry.Text).ToArray();
            foreach (string context in new[] { "", "the ", "the quick ", "a slow ", "unknown missing " })
                foreach (string prefix in new[] { "", "b", "br", "f", "q", "a" })
                {
                    var expected = TrieSuggestions.Predict(models[0], context, prefix, default, language: "en");
                    foreach (var model in models.Skip(1).Take(2))
                        Check(TrieSuggestions.Predict(model, context, prefix, default, language: "en").SequenceEqual(expected),
                            "Native v4 count completion changed legacy rankings");
                    var (previous, older) = TrieSuggestions.ReadContext(context, "en");
                    var scored = models[^1];
                    var exhaustive = vocabulary.Where(word => word.StartsWith(prefix, StringComparison.Ordinal))
                        .OrderByDescending(word => scored.ConditionalLogProbability(word, previous, older)).ThenBy(word => Encoding.UTF8.GetBytes(word), ByteOrder.Instance).Take(3).ToArray();
                    var actual = TrieSuggestions.Predict(scored, context, prefix, default, language: "en").Select(word => word.ToLowerInvariant());
                    Check(actual.SequenceEqual(exhaustive), "Fresh native MKN model's bounded completion differs from exhaustive conditional probability");
                }
            foreach (var model in models)
            {
                Check(TrieCorrections.Find(model, keyboard, "quik", "the", "", 3, word => word, [], default).Contains("quick"),
                    "Fresh native model did not recover deletion in simulator");
                Check(TrieCorrections.Find(model, keyboard, "quick", "the", "", 3, word => word, [], default).Count == 0,
                    "Fresh native model edited a correctly spelled word");
            }
            Console.WriteLine("Fresh native v2/v3/v4 counts and v4 MKN models: exact entries, NGIX parity, completion rankings and correction checks passed.");
        }
        finally { Directory.Delete(work, recursive: true); }
    }
}
