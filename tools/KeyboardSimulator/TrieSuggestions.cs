using System.Buffers;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using NgramTrie;
using NgramTrie.Reader;

namespace KeyboardSimulator;

internal static class TrieSuggestions
{
    private const int SuggestionCount = 3;
    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);
    private static readonly IComparer<Branch> BranchOrder = Comparer<Branch>.Create((left, right) =>
    {
        int byMass = left.Node.Mass.CompareTo(right.Node.Mass);
        return byMass != 0 ? byMass : right.Node.Label.CompareTo(left.Node.Label);
    });

    public static IReadOnlyList<string> Predict(TrieModel trie, string textBeforeWord, string prefix,
        CancellationToken cancellation, KeyboardNeighborMap? keyboard = null, string? language = null)
    {
        cancellation.ThrowIfCancellationRequested();
        bool includeMarks = language == "es" || WordNormalization.UsesListedSpelling(language) || WordNormalization.UsesUnicodeTokens(language);
        foreach (Rune rune in prefix.EnumerateRunes())
        {
            if (!WordNormalization.IsWordRune(rune, includeMarks) && rune.Value is not ('\'' or '’') && !SuggestionText.IsJoiner(rune, language)) return [];
        }

        var (previous, older) = ReadContext(textBeforeWord.AsSpan(), language);
        string normalizedPrefix = WordNormalization.NormalizeWord(prefix, language);
        byte[] prefixBytes = Utf8.GetBytes(normalizedPrefix);
        // A capital sigma at the caret can end this word or precede more letters.
        string? continuation = language == "el" && prefix.Length > 1 && prefix[^1] == 'Σ'
            ? WordNormalization.NormalizeWord(prefix, language, isPrefix: true) : null;
        byte[]? continuationBytes = continuation is not null ? Utf8.GetBytes(continuation) : null;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var result = new List<string>(SuggestionCount);
        bool upperSuffix = IsAllCapsPrefix(prefix);

        if (trie.HasLanguageScores)
        {
            var best = new List<ScoredMatch>(SuggestionCount);
            AddScoredPrefix(normalizedPrefix, prefixBytes);
            if (continuation is not null && continuationBytes is not null) AddScoredPrefix(continuation, continuationBytes);
            foreach (var match in best) { seen.Add(match.Text); result.Add(match.Text); }
            if (keyboard is not null && result.Count < SuggestionCount)
                result.AddRange(TrieCorrections.Find(trie, keyboard, normalizedPrefix, previous, older,
                    SuggestionCount - result.Count, word => DisplayCorrection(word, prefix, upperSuffix, language), seen, cancellation));
            return result;

            void AddScoredPrefix(string term, byte[] bytes)
            {
                int? two = previous.Length == 0 ? null : trie.FindPrefix(previous);
                int? three = older.Length == 0 || previous.Length == 0 ? null : trie.FindPrefix(older + " " + previous);
                double threeBackoff = Backoff(three), twoBackoff = Backoff(two);
                AddLayer(three, 0, null, null);
                AddLayer(two, threeBackoff, three, null);
                AddLayer(null, threeBackoff + twoBackoff, three, two, unigram: true);

                double Backoff(int? history) => history is int handle && (trie.GetNode(handle).ScoreFlags & 4) != 0
                    ? TrieModel.DecodeLogScore(trie.GetNode(handle).LogBackoff) : 0;
                bool Observed(int? history, ReadOnlySpan<byte> word)
                {
                    int? space = history is int start ? trie.FindChild(start, (byte)' ') : null;
                    int? node = space is int after ? trie.FollowFrom(after, word) : null;
                    return node is int found && (trie.GetNode(found).ScoreFlags & 2) != 0;
                }
                void AddLayer(int? history, double factor, int? excludeFirst, int? excludeSecond, bool unigram = false)
                {
                    if (double.IsNegativeInfinity(factor)) return;
                    int? root = unigram ? (int)TrieRoot.Unigrams : history is int start ? trie.FindChild(start, (byte)' ') : null;
                    int? found = root is int after ? trie.FollowFrom(after, bytes) : null;
                    if (found is not int initial) return;
                    var path = new List<byte>(bytes);
                    var pending = new List<Branch> { new(trie.GetNode(initial), path.Count, false) };
                    while (pending.Count > 0)
                    {
                        cancellation.ThrowIfCancellationRequested();
                        var branch = pending[^1]; pending.RemoveAt(pending.Count - 1);
                        double bound = TrieModel.DecodeLogScore(branch.Node.MaximumLogProbability) + factor;
                        if (double.IsNegativeInfinity(bound) || best.Count == SuggestionCount && bound < best[^1].Score) continue;
                        path.RemoveRange(branch.ParentLength, path.Count - branch.ParentLength);
                        if (branch.AppendLabel) path.Add(branch.Node.Label);
                        int childrenStart = pending.Count;
                        for (int ordinal = 0; ordinal < branch.Node.ChildCount; ordinal++)
                        {
                            var child = trie.GetNode(branch.Node.ChildAt(ordinal));
                            if (child.Label != (byte)' ') pending.Add(new(child, path.Count, true));
                        }
                        if (pending.Count - childrenStart > 1)
                            pending.Sort(childrenStart, pending.Count - childrenStart, ScoredBranchOrder);
                        if (path.Count == 0 || (branch.Node.ScoreFlags & 2) == 0) continue;
                        ReadOnlySpan<byte> word = CollectionsMarshal.AsSpan(path);
                        if (Observed(excludeFirst, word) || Observed(excludeSecond, word)) continue;
                        double score = TrieModel.DecodeLogScore(branch.Node.LogProbability) + factor;
                        if (double.IsNegativeInfinity(score)) continue;
                        string decoded;
                        try { decoded = Utf8.GetString(word); } catch (DecoderFallbackException) { continue; }
                        string text = ApplyCase(decoded, prefix, term.Length, upperSuffix, previous.Length == 0, language);
                        int duplicate = best.FindIndex(value => value.Text == text);
                        if (duplicate >= 0)
                        {
                            if (best[duplicate].Score >= score) continue;
                            best.RemoveAt(duplicate);
                        }
                        int position = 0;
                        while (position < best.Count && (best[position].Score > score || best[position].Score == score &&
                            best[position].Path.AsSpan().SequenceCompareTo(word) < 0)) position++;
                        if (position >= SuggestionCount) continue;
                        if (best.Count == SuggestionCount) best.RemoveAt(best.Count - 1);
                        best.Insert(position, new(text, word.ToArray(), score));
                    }
                }
            }
        }

        if (older.Length != 0 && previous.Length != 0)
            AddSuggestions($"{older} {previous} ", TrieRoot.Combined);
        if (previous.Length != 0)
            AddSuggestions($"{previous} ", TrieRoot.Combined);
        AddSuggestions("", TrieRoot.Unigrams);
        if (keyboard is not null && result.Count < SuggestionCount)
            result.AddRange(TrieCorrections.Find(trie, keyboard, normalizedPrefix, previous, older,
                SuggestionCount - result.Count, word => DisplayCorrection(word, prefix, upperSuffix, language), seen, cancellation));
        return result;

        void AddSuggestions(string context, TrieRoot root)
        {
            AddPrefix(context, root, normalizedPrefix, prefixBytes);
            if (continuation is not null && continuationBytes is not null)
                AddPrefix(context, root, continuation, continuationBytes);
        }

        void AddPrefix(string context, TrieRoot root, string term, byte[] bytes)
        {
            int remaining = SuggestionCount - result.Count;
            if (remaining == 0 || trie.FindPrefix(context + term, root) is not int index) return;
            foreach (var match in FindBestWords(trie, index, bytes, seen, remaining,
                         word => ApplyCase(word, prefix, term.Length, upperSuffix, previous.Length == 0, language), cancellation))
            {
                seen.Add(match.Text);
                result.Add(match.Text);
            }
        }
    }

    internal static int LastSentenceSeparator(ReadOnlySpan<char> text) => text.LastIndexOfAny(".!?\r\n\u2028\u2029\u061f\u06d4\u0964\u0965\u3002\uff01\uff1f");

    internal static (string Previous, string Older) ReadContext(ReadOnlySpan<char> textBeforeWord, string? language = null)
    {
        var sentence = textBeforeWord[(LastSentenceSeparator(textBeforeWord) + 1)..];
        bool includeMarks = language == "es" || WordNormalization.UsesListedSpelling(language) || WordNormalization.UsesUnicodeTokens(language);
        string previous = WordNormalization.NormalizeWord(TakeLastWord(ref sentence, includeMarks, language), language);
        return (previous, WordNormalization.NormalizeWord(TakeLastWord(ref sentence, includeMarks, language), language));
    }

    internal static string DisplayCorrection(string word, string typed, string? language = null) =>
        DisplayCorrection(word, typed, IsAllCapsPrefix(typed), language);

    private static string DisplayCorrection(string word, string typed, bool upperSuffix, string? language) =>
        upperSuffix ? Uppercase(word, language) :
        ApplyCase(word, "", 0, upperSuffix: false, typed.Length > 0 && Rune.IsUpper(Rune.GetRuneAt(typed, 0)), language);

    private static List<Match> FindBestWords(TrieModel trie, int start, byte[] prefix, HashSet<string> seen, int count,
        Func<string, string> display, CancellationToken cancellation)
    {
        var path = new List<byte>(prefix);
        var stack = new List<Branch> { new(trie.GetNode(start), prefix.Length, AppendLabel: false) };
        var best = new List<Match>(count);
        while (stack.Count != 0)
        {
            cancellation.ThrowIfCancellationRequested();
            var branch = stack[^1];
            stack.RemoveAt(stack.Count - 1);
            ulong lowerBound = best.Count == count ? best[^1].Mass : 0;
            if (branch.Node.Mass < lowerBound) continue;

            // The stack performs depth-first traversal. Siblings share their parent's path.
            path.RemoveRange(branch.ParentLength, path.Count - branch.ParentLength);
            if (branch.AppendLabel) path.Add(branch.Node.Label);
            if (best.Count == count && Compare(branch.Node.Mass, CollectionsMarshal.AsSpan(path), best[^1]) >= 0)
                continue;

            // Terminal mass plus space-child mass is the whole-word mass. Other
            // children continue the current word and are bounded by their node mass.
            ulong wordMass = branch.Node.Mass;
            int childrenStart = stack.Count;
            for (int ordinal = 0; ordinal < branch.Node.ChildCount; ordinal++)
            {
                int index = branch.Node.ChildAt(ordinal);
                var child = trie.GetNode(index);
                if (child.Label == (byte)' ') continue;
                wordMass -= child.Mass;
                if (child.Mass >= lowerBound)
                    stack.Add(new(child, path.Count, AppendLabel: true));
            }
            if (stack.Count - childrenStart > 1)
                stack.Sort(childrenStart, stack.Count - childrenStart, BranchOrder);
            if (wordMass == 0 || path.Count == 0) continue;

            ReadOnlySpan<byte> bytes = CollectionsMarshal.AsSpan(path);
            int position = 0;
            while (position < best.Count && Compare(wordMass, bytes, best[position]) > 0) position++;
            if (position == count) continue;

            // The reader validates structural records, so isolate malformed UTF-8
            // words without treating bytes from different paths as the same word.
            string word;
            try { word = Utf8.GetString(bytes); }
            catch (DecoderFallbackException) { continue; }
            string text = display(word);
            if (seen.Contains(text)) continue;
            int duplicate = best.FindIndex(match => match.Text == text);
            if (duplicate >= 0)
            {
                if (position > duplicate) continue;
                best.RemoveAt(duplicate);
            }
            if (best.Count == count) best.RemoveAt(count - 1);
            best.Insert(position, new(text, bytes.ToArray(), wordMass));
        }
        return best;
    }

    private static int Compare(ulong mass, ReadOnlySpan<byte> path, Match other)
    {
        int byMass = other.Mass.CompareTo(mass);
        return byMass != 0 ? byMass : path.SequenceCompareTo(other.Path);
    }

    private static string TakeLastWord(ref ReadOnlySpan<char> text, bool includeMarks, string? language)
    {
        int end = text.Length;
        while (end > 0 && !ReadWordRune(text[..end], includeMarks, out int width)) end -= Math.Max(width, 1);

        if (language == "zh" && Rune.DecodeLastFromUtf16(text[..end], out var last, out int hanWidth) == OperationStatus.Done && WordNormalization.IsHan(last)) {
            string character = text[(end - hanWidth)..end].ToString();
            text = text[..(end - hanWidth)];
            return character;
        }

        int start = end;
        while (start > 0)
        {
            if (language == "zh" && Rune.DecodeLastFromUtf16(text[..start], out var rune, out _) == OperationStatus.Done && WordNormalization.IsHan(rune)) break;
            if (ReadWordRune(text[..start], includeMarks, out int width))
                start -= width;
            else if ((text[start - 1] is '\'' or '’' ||
                language is "ar" or "hi" or "bn" or "ur" && text[start - 1] is '\u200c' or '\u200d') &&
                ReadWordRune(text[..(start - 1)], includeMarks, out width))
                start -= width + 1;
            else
                break;
        }

        string word = text[start..end].ToString();
        text = text[..start];
        return word;
    }

    private static bool ReadWordRune(ReadOnlySpan<char> text, bool includeMarks, out int width) =>
        Rune.DecodeLastFromUtf16(text, out var rune, out width) == OperationStatus.Done && WordNormalization.IsWordRune(rune, includeMarks);

    private static string ApplyCase(string word, string prefix, int normalizedPrefixLength, bool upperSuffix, bool sentenceStart, string? language)
    {
        if (prefix.Length != 0)
        {
            string suffix = word[normalizedPrefixLength..];
            return prefix + (upperSuffix ? Uppercase(suffix, language) : suffix);
        }
        if (!sentenceStart) return word;
        var initial = Rune.GetRuneAt(word, 0);
        return Uppercase(initial.ToString(), language) + word[initial.Utf16SequenceLength..];
    }

    private static string Uppercase(string word, string? language) => language switch
    {
        "de" => word.ToUpperInvariant().Replace('ß', 'ẞ'),
        "el" => word.ToUpperInvariant().Replace("ΐ", "Ϊ́").Replace("ΰ", "Ϋ́"),
        _ => word.ToUpperInvariant(),
    };

    private static bool IsAllCapsPrefix(string prefix)
    {
        int capitalLetters = 0;
        foreach (Rune rune in prefix.EnumerateRunes())
        {
            switch (Rune.GetUnicodeCategory(rune))
            {
                case UnicodeCategory.LowercaseLetter:
                    return false;
                case UnicodeCategory.UppercaseLetter or UnicodeCategory.TitlecaseLetter:
                    capitalLetters++;
                    break;
            }
        }
        return capitalLetters > 1;
    }

    internal static string Fold(string value) => WordNormalization.Fold(value);

    private readonly record struct Branch(TrieNode Node, int ParentLength, bool AppendLabel);
    private readonly record struct Match(string Text, byte[] Path, ulong Mass);
    private readonly record struct ScoredMatch(string Text, byte[] Path, double Score);
    private static readonly IComparer<Branch> ScoredBranchOrder = Comparer<Branch>.Create((left, right) =>
    {
        int score = left.Node.MaximumLogProbability.CompareTo(right.Node.MaximumLogProbability);
        return score != 0 ? score : right.Node.Label.CompareTo(left.Node.Label);
    });
}
