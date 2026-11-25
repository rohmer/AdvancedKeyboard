using System.Globalization;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using GBUTest;
using NgramTrie.Reader;

namespace KeyboardParity;

internal static class Program
{
    private static readonly (string Name, string Typed, string Previous, string Older)[] CorrectionCases =
    [
        ("transposition", "teh", "", ""),
        ("contextual-transposition", "teh", "in", ""),
        ("two-word-context", "teh", "in", "was"),
        ("alternate-transposition", "adn", "", ""),
        ("adjacent-keys", "hellp", "", ""),
        ("common-transposition", "recieve", "to", "able"),
        ("common-misspelling", "definately", "", ""),
        ("rare-input", "qzqzqzqz", "", ""),
        ("correct-word", "the", "", ""),
        ("short-input", "a", "", ""),
        ("digit", "3", "", ""),
        ("ordinal", "21st", "", ""),
        ("calendar", "monday", "", ""),
        ("number-word", "twenty", "", ""),
        ("unicode-input", "cafÃ©", "", ""),
    ];

    private static readonly (string Name, string Context, string Prefix, string Previous, string Older)[] PrefixCases =
    [
        ("sentence-start", "", "", "", ""),
        ("single-letter", "", "t", "", ""),
        ("continued-word", "", "qu", "", ""),
        ("all-caps", "", "TH", "", ""),
        ("one-word-context", "the ", "", "the", ""),
        ("two-word-context", "in the ", "", "the", "in"),
        ("other-trigram-context", "one of ", "", "of", "one"),
        ("context-and-prefix", "in the ", "s", "the", "in"),
        ("capitalized-context", "United States ", "o", "states", "united"),
        ("period-reset", "in the. ", "", "", ""),
        ("newline-reset", "in the!\n", "t", "", ""),
        ("unavailable-prefix", "in the ", "qxz", "the", "in"),
    ];

    private static int Main(string[] args)
    {
        if (args.Length is not (3 or 4))
        {
            Console.Error.WriteLine("Usage: KeyboardParity <paired-file.txt> <model.ngtr> <reference.tsv> [queries.h]");
            return 2;
        }

        try
        {
            string pairsPath = Path.GetFullPath(args[0]);
            string modelPath = Path.GetFullPath(args[1]);
            string output = Path.GetFullPath(args[2]);
            string? header = args.Length == 4 ? Path.GetFullPath(args[3]) : null;
            var comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
            var paths = new[] { pairsPath, modelPath, output, header }.OfType<string>().ToArray();
            if (paths.Distinct(comparer).Count() != paths.Length)
                throw new ArgumentException("Choose separate paths for the paired file, trie, and reference outputs.");

            var trie = TrieModel.Load(modelPath);
            var pairs = SentenceFuzzer.ReadPairs(pairsPath);
            var regression = TypingTester.RunRegression(pairsPath, modelPath);
            var desktop = new DesktopReference();
            var words = ReadWords(desktop, pairs);
            var edits = words.Where(word => word.Expected != word.Typed).ToArray();
            var correctionCases = CorrectionCases.Concat(Choose(edits, 96 - CorrectionCases.Length).Select(word =>
                (Name: $"pair-{word.Pair}-word-{word.Position}", word.Typed, word.Previous, word.Older)))
                .Select(query => new CorrectionQuery(query.Name, query.Typed, query.Previous, query.Older,
                    desktop.Correct(trie, query.Typed, query.Previous, query.Older))).ToArray();
            var contextual = words.Where(word => word.Typed.Length >= 3 && word.Older.Length > 0 &&
                word.Typed.All(char.IsAsciiLetterLower) && word.Previous.All(char.IsAsciiLetterLower) &&
                word.Older.All(char.IsAsciiLetterLower)).ToArray();
            var prefixCases = PrefixCases.Concat(Choose(contextual, 48 - PrefixCases.Length).Select((word, index) =>
                (Name: $"pair-{word.Pair}-word-{word.Position}", Context: $"{word.Older} {word.Previous} ", Prefix: word.Typed[..(index % 4)],
                    word.Previous, word.Older)))
                .Select(query => new PrefixQuery(query.Name, desktop.Fold(query.Prefix), query.Previous, query.Older,
                    desktop.Predict(trie, query.Context, query.Prefix).Select(desktop.Fold).ToArray())).ToArray();
            string modelHash = Sha256(modelPath);
            Write(output, writer =>
            {
                writer.WriteLine("# keyboard-parity-v1");
                writer.WriteLine(FormattableString.Invariant(
                    $"M\t{modelHash}\t{new FileInfo(modelPath).Length}\t{trie.Version}\t{trie.NodeCount}\t{Sha256(pairsPath)}\t{regression.Pairs}\t{regression.TotalWords}"));
                foreach (var query in correctionCases)
                    writer.WriteLine(FormattableString.Invariant(
                        $"A\t{query.Name}\t{Hex(query.Typed)}\t{Hex(query.Previous)}\t{Hex(query.Older)}\t{Hex(query.Correction?.Word ?? "")}\t{query.Correction?.Probability:R}"));
                foreach (var query in prefixCases)
                    writer.WriteLine(string.Join('\t', new[] { "P", query.Name, Hex(query.Prefix), Hex(query.Previous), Hex(query.Older) }
                        .Concat(query.Words.Select(Hex))));
                foreach (var row in regression.Results)
                    writer.WriteLine(FormattableString.Invariant(
                        $"R\t{row.Threshold:R}\t{row.TotalWords}\t{row.Score}\t{row.Good}\t{row.Bad}\t{row.Ugly}\t{row.KeptCorrect}\t{row.Missed}"));
                foreach (var word in words)
                    writer.WriteLine(FormattableString.Invariant(
                        $"W\t{word.Pair}\t{word.Position}\t{(word.StartsSentence ? 1 : 0)}\t{Hex(word.Expected)}\t{Hex(word.Typed)}"));
            });
            if (header is not null) Write(header, writer => WriteHeader(writer, modelHash, correctionCases, prefixCases));
            Console.WriteLine($"{pairs.Length:N0} pairs, {regression.TotalWords:N0} words, {regression.Results.Count:N0} thresholds, and " +
                              $"{correctionCases.Length} correction and {prefixCases.Length} prefix queries written to {output}");
            return 0;
        }
        catch (Exception error) when (error is ArgumentException or IOException or InvalidDataException or
                                      UnauthorizedAccessException or TargetInvocationException)
        {
            Console.Error.WriteLine($"KeyboardParity: {error.InnerException?.Message ?? error.Message}");
            return 1;
        }
    }

    private static WordCase[] ReadWords(DesktopReference desktop, SentencePair[] pairs)
    {
        var cases = new List<WordCase>();
        for (int pair = 0; pair < pairs.Length; pair++)
        {
            string previous = "";
            string older = "";
            int position = 0;
            foreach (var word in desktop.Words(pairs[pair], pair + 1))
            {
                if (word.StartsSentence) previous = older = "";
                cases.Add(new(pair + 1, ++position, word.Expected, word.Typed, word.StartsSentence, previous, older));
                older = previous;
                previous = word.Typed;
            }
        }
        return [.. cases];
    }

    private static IEnumerable<T> Choose<T>(T[] values, int count)
    {
        count = Math.Min(count, values.Length);
        for (int index = 0; index < count; index++) yield return values[(int)((long)index * values.Length / count)];
    }

    private static void Write(string path, Action<StreamWriter> write)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = $"{path}.{Guid.NewGuid():N}.tmp";
        try
        {
            using (var writer = new StreamWriter(temporary, false, new UTF8Encoding(false)) { NewLine = "\n" }) write(writer);
            File.Move(temporary, path, overwrite: true);
        }
        finally { File.Delete(temporary); }
    }

    private static void WriteHeader(StreamWriter writer, string modelHash, CorrectionQuery[] corrections, PrefixQuery[] prefixes)
    {
        writer.WriteLine("""
            #pragma once
            #include <stddef.h>

            namespace keyboard_parity {
            struct AutocorrectCase {
                const char *name, *typed, *previous, *older, *word;
                double probability;
            };
            struct PrefixCase {
                const char *name, *prefix, *previous, *older, *words[3];
                size_t count;
            };
            """);
        writer.WriteLine($"inline constexpr char kModelSha256[] = \"{modelHash}\";");
        writer.WriteLine("inline constexpr AutocorrectCase kAutocorrectCases[] = {");
        foreach (var query in corrections)
            writer.WriteLine(FormattableString.Invariant($"    {{\"{query.Name}\", {Literal(query.Typed)}, {Literal(query.Previous)}, {Literal(query.Older)}, {Literal(query.Correction?.Word ?? "")}, {query.Correction?.Probability ?? 0:R}}},"));
        writer.WriteLine("};");
        writer.WriteLine("inline constexpr PrefixCase kPrefixCases[] = {");
        foreach (var query in prefixes)
        {
            string words = string.Join(", ", query.Words.Select(Literal).Concat(Enumerable.Repeat("nullptr", 3 - query.Words.Length)));
            writer.WriteLine($"    {{\"{query.Name}\", {Literal(query.Prefix)}, {Literal(query.Previous)}, {Literal(query.Older)}, {{{words}}}, {query.Words.Length}}},");
        }
        writer.WriteLine("};");
        writer.WriteLine("} // namespace keyboard_parity");
    }

    private static string Literal(string value) =>
        "\"" + string.Concat(Encoding.UTF8.GetBytes(value).Select(octet => $"\\x{octet:x2}")) + "\"";

    private sealed record WordCase(int Pair, int Position, string Expected, string Typed, bool StartsSentence,
        string Previous, string Older);
    private sealed record CorrectionQuery(string Name, string Typed, string Previous, string Older,
        (string Word, double Probability)? Correction);
    private sealed record PrefixQuery(string Name, string Prefix, string Previous, string Older, string[] Words);

    private static string Sha256(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(file));
    }

    private static string Hex(string value) => Convert.ToHexString(Encoding.UTF8.GetBytes(value));

    // The reference calls the desktop scorer's own internal methods. This keeps
    // tokenization, capitalization, and tie-breaking in the code being compared.
    private sealed class DesktopReference
    {
        private readonly object keyboard;
        private readonly MethodInfo correction;
        private readonly MethodInfo predict;
        private readonly MethodInfo fold;
        private readonly MethodInfo readWords;
        private readonly PropertyInfo expected;
        private readonly PropertyInfo typed;
        private readonly PropertyInfo startsSentence;

        public DesktopReference()
        {
            var tester = typeof(TypingTester);
            var simulator = Assembly.Load("KeyboardSimulator");
            keyboard = tester.Assembly.GetType("GBUTest.TestData", throwOnError: true)!
                .GetMethod("LoadNeighbors", BindingFlags.Public | BindingFlags.Static)!
                .Invoke(null, null)!;
            correction = simulator.GetType("KeyboardSimulator.TrieCorrections", throwOnError: true)!
                .GetMethod("BestAutocorrection", BindingFlags.Public | BindingFlags.Static)!;
            predict = simulator.GetType("KeyboardSimulator.TrieSuggestions", throwOnError: true)!
                .GetMethod("Predict", BindingFlags.Public | BindingFlags.Static)!;
            fold = predict.DeclaringType!.GetMethod("Fold", BindingFlags.NonPublic | BindingFlags.Static)!;
            readWords = tester.GetMethod("ReadWords", BindingFlags.NonPublic | BindingFlags.Static)!;
            var word = readWords.ReturnType.GetElementType()!;
            expected = word.GetProperty("FoldedExpected")!;
            typed = word.GetProperty("FoldedTyped")!;
            startsSentence = word.GetProperty("StartsSentence")!;
        }

        public (string Word, double Probability)? Correct(TrieModel model, string input, string previous, string older) =>
            correction.Invoke(null, [model, keyboard, input, previous, older, CancellationToken.None])
                is ValueTuple<string, double> candidate ? candidate : null;

        public IReadOnlyList<string> Predict(TrieModel model, string context, string prefix) =>
            (IReadOnlyList<string>)predict.Invoke(null, [model, context, prefix, CancellationToken.None, null, null])!;

        public string Fold(string word) => (string)fold.Invoke(null, [word])!;

        public IEnumerable<(string Expected, string Typed, bool StartsSentence)> Words(SentencePair pair, int number)
        {
            foreach (var word in (Array)readWords.Invoke(null, [pair, number, CancellationToken.None])!)
                yield return ((string)expected.GetValue(word)!, (string)typed.GetValue(word)!, (bool)startsSentence.GetValue(word)!);
        }
    }
}
