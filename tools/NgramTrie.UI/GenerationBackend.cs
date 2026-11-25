using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace NgramTrie.UI;

internal static class GenerationBackend
{
    internal sealed record Metrics(int EffectiveWorkers, bool TrainingCacheHit, long IndexBytes,
        long LogicalNodes, long PhysicalNodes, IReadOnlyList<string> ProfilePaths);

    internal static bool RequiresNativeSettings(IEnumerable<string> arguments) => arguments.TakeWhile(value => value != "--")
        .Any(value => value is "--format" or "--smoothing" or "--memory-mib" or "--profile-sizes");
    public static string? FindNativeHelper()
    {
        string name = OperatingSystem.IsWindows() ? "UICreatorModelGen.exe" : "UICreatorModelGen";
        if (Environment.GetEnvironmentVariable("UICREATOR_MODELGEN") is { Length: > 0 } configured)
            return File.Exists(configured) ? Path.GetFullPath(configured) : null;
        string adjacent = Path.Combine(AppContext.BaseDirectory, name);
        if (File.Exists(adjacent)) return adjacent;
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            string candidate = Path.Combine(directory.FullName, "tools", "NativeTrieGenerator", "bin", name);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }
    public static string Describe() => FindNativeHelper() is null
        ? "Managed C# fallback — native helper is unavailable. Radix, smoothing, RAM budgets and batch profiles require the native generator."
        : "Native generator: " + FindNativeHelper();

    private static async Task VerifyCapabilities(string helper, string[] arguments, CancellationToken cancellation)
    {
        var start = new ProcessStartInfo(helper)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(helper)!,
        };
        start.ArgumentList.Add("--capabilities");
        using var process = Process.Start(start) ?? throw new IOException("The native generator could not start: " + helper);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(TimeSpan.FromSeconds(10));
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        try { await process.WaitForExitAsync(deadline.Token); }
        catch (OperationCanceledException)
        {
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            cancellation.ThrowIfCancellationRequested();
            throw new IOException("The native generator did not report its capabilities: " + helper);
        }
        string text = await output; await errors;
        try
        {
            if (process.ExitCode != 0) throw new JsonException();
            using var document = JsonDocument.Parse(text);
            var root = document.RootElement;
            bool Listed(string key, string value) => root.TryGetProperty(key, out var values) && values.ValueKind == JsonValueKind.Array &&
                values.EnumerateArray().Any(item => item.ValueKind == JsonValueKind.String && item.GetString() == value);
            for (int i = 0; i < arguments.Length && arguments[i] != "--"; i++)
            {
                string flag = arguments[i];
                if (flag is not ("--format" or "--smoothing" or "--memory-mib" or "--profile-sizes")) continue;
                if (++i >= arguments.Length) throw new ArgumentException("Missing value for " + flag);
                bool supported = flag switch
                {
                    "--format" => Listed("formats", arguments[i]),
                    "--smoothing" => Listed("smoothing", arguments[i]),
                    "--memory-mib" => root.TryGetProperty("memoryBudget", out var memory) && memory.ValueKind == JsonValueKind.True,
                    _ => root.TryGetProperty("batchProfiles", out var profiles) && profiles.ValueKind == JsonValueKind.True,
                };
                if (!supported) throw new JsonException();
            }
        }
        catch (JsonException)
        {
            throw new IOException("This native generator does not support the selected settings. Rebuild or update it: " + helper);
        }
    }

    public static async Task<GenerationResult> GenerateAsync(string[] arguments, Action<GenerationProgress>? report,
        CancellationToken cancellation, Action<Metrics>? completed = null)
    {
        string? helper = FindNativeHelper();
        if (helper is null)
        {
            if (RequiresNativeSettings(arguments)) throw new InvalidOperationException(
                "The selected model format, scoring, RAM budget or batch profiles require UICreatorModelGen. Build the native generator or install it beside Corpus Workbench.");
            return await Task.Run(() => Generator.Generate(arguments, report, cancellation));
        }
        cancellation.ThrowIfCancellationRequested();
        if (RequiresNativeSettings(arguments)) await VerifyCapabilities(helper, arguments, cancellation);
        var start = new ProcessStartInfo(helper)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = Path.GetDirectoryName(helper)!,
        };
        foreach (string argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new IOException("The native model generator could not start.");
        var errors = process.StandardError.ReadToEndAsync();
        using var registration = cancellation.Register(() =>
        {
            try { if (!process.HasExited) { process.StandardInput.WriteLine("cancel"); process.StandardInput.Flush(); } }
            catch (Exception error) when (error is IOException or InvalidOperationException) { }
        });
        GenerationResult? result = null;
        int workers = 0; bool trainingCacheHit = false; long indexBytes = 0, logicalNodes = 0, physicalNodes = 0;
        var profiles = new List<string>();
        while (await process.StandardOutput.ReadLineAsync() is { } line)
        {
            var fields = line.Split('\t');
            if (fields is ["@progress", var percent, var stage] && int.TryParse(percent, NumberStyles.None, CultureInfo.InvariantCulture, out int number))
                report?.Invoke(new(stage, Math.Clamp(number, 0, 100)));
            else if (fields is ["@result", var unigrams, var bigrams, var trigrams, var bytes])
                result = new(int.Parse(unigrams, CultureInfo.InvariantCulture), int.Parse(bigrams, CultureInfo.InvariantCulture),
                    int.Parse(trigrams, CultureInfo.InvariantCulture), long.Parse(bytes, CultureInfo.InvariantCulture));
            else if (fields.Length > 2 && fields[0] == "@metrics")
            {
                for (int i = 1; i + 1 < fields.Length; i += 2)
                {
                    if (!long.TryParse(fields[i + 1], NumberStyles.Integer, CultureInfo.InvariantCulture, out long value)) continue;
                    switch (fields[i])
                    {
                        case "workers": workers = checked((int)value); break;
                        case "trainingCacheHit": trainingCacheHit = value != 0; break;
                        case "indexBytes": indexBytes = value; break;
                        case "logicalNodes": logicalNodes = value; break;
                        case "physicalNodes": physicalNodes = value; break;
                    }
                }
            }
            else if (fields is ["@profile", var path]) profiles.Add(path);
        }
        await process.WaitForExitAsync();
        string errorText = (await errors).Trim();
        // A successful publication wins a late Stop, just as the managed port.
        if (process.ExitCode == 0 && result is not null)
        {
            completed?.Invoke(new(workers, trainingCacheHit, indexBytes, logicalNodes, physicalNodes, profiles.ToArray()));
            return result;
        }
        if (cancellation.IsCancellationRequested) throw new OperationCanceledException(cancellation);
        throw new IOException(errorText.Length == 0 ? $"Native generation failed (exit {process.ExitCode})." : errorText);
    }
}
