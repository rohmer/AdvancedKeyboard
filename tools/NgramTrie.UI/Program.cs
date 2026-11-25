using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace NgramTrie.UI;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new GeneratorForm());
    }
}

internal sealed class GeneratorForm : Form
{
    private sealed record LanguageOption(string Code, string Label)
    {
        public override string ToString() => Label;
    }

    private static readonly LanguageOption[] Languages =
        [new("en", "English"), new("es", "Spanish"), new("fr", "French"), new("de", "German"), new("el", "Greek"),
         new("ar", "Arabic"), new("hi", "Hindi"), new("zh", "Mandarin Chinese"), new("bn", "Bengali"),
         new("id", "Indonesian"), new("ur", "Urdu"), new("ru", "Russian")];
    private readonly TextBox directories = new()
    {
        Multiline = true, AcceptsReturn = true, WordWrap = false, ScrollBars = ScrollBars.Vertical,
        MinimumSize = new Size(0, 140), Dock = DockStyle.Fill, PlaceholderText = @"D:\Corpus\foxnews",
    };
    private readonly CheckBox includeSubdirectories = new() { Checked = true, Text = "Include subdirectories", AutoSize = true };
    private readonly TextBox output = new() { Dock = DockStyle.Fill };
    private readonly ComboBox language = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 150 };
    private readonly TextBox bytes = new() { Text = "262144", PlaceholderText = "Unlimited" };
    private readonly TextBox unigrams = new() { PlaceholderText = "Automatic" };
    private readonly TextBox bigrams = new() { PlaceholderText = "Automatic" };
    private readonly TextBox trigrams = new() { PlaceholderText = "Automatic" };
    private readonly NumericUpDown workers = new() { Minimum = 1, Maximum = 1024, Value = Math.Min(Environment.ProcessorCount, 1024) };
    private readonly CheckBox useCache = new() { Checked = true, Text = "Reuse saved counts", AutoSize = true };
    private readonly CheckBox lowMemory = new() { Checked = true, Text = "Keep counts on disk", AutoSize = true };
    private readonly ComboBox format = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 240 };
    private readonly ComboBox smoothing = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220 };
    private readonly NumericUpDown memory = new() { Minimum = 16, Maximum = 65536, Value = 512, Width = 120 };
    private readonly TextBox profiles = new() { Dock = DockStyle.Fill, PlaceholderText = "16384, 262144 (optional)" };
    private readonly Button add = new() { Text = "Add folder…", AutoSize = true };
    private readonly Button browse = new() { Text = "Browse…", AutoSize = true };
    private readonly Button generate = new() { Text = "Generate trie", AutoSize = true };
    private readonly Button stop = new() { Text = "Stop", AutoSize = true, Enabled = false };
    private readonly ProgressBar bar = new() { Dock = DockStyle.Fill };
    private readonly Label timing = new() { Dock = DockStyle.Fill, AutoSize = true, Text = "Ready" };
    private readonly Label backend = new() { Dock = DockStyle.Fill, AutoSize = true };
    private readonly TextBox status = new()
    {
        Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical,
        BorderStyle = BorderStyle.None, BackColor = SystemColors.Control, TabStop = false,
    };
    private readonly Stopwatch clock = new();
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 1000 };
    private GenerationProgress progress = new("Ready", 0);
    private CancellationTokenSource? current;
    private readonly UiSettingsStore settings;

    internal bool IsRunning => current is not null;
    internal event Action<string, string>? TrieCreated;

    internal void CancelWork() => current?.Cancel();

    internal bool UseCorpora(IEnumerable<string> files, string languageCode)
    {
        var selectedLanguage = Languages.FirstOrDefault(option => option.Code == languageCode);
        if (IsRunning || selectedLanguage is null) return false;
        directories.Lines = files.Select(Path.GetFullPath)
            .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        language.SelectedItem = selectedLanguage;
        return true;
    }

    public GeneratorForm() : this(new UiSettingsStore()) { }

    internal GeneratorForm(UiSettingsStore settings)
    {
        this.settings = settings;
        Text = "NGramTrie Generator";
        ClientSize = new Size(900, 740);
        MinimumSize = new Size(700, 570);
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Font;
        output.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "model.ngtr");
        language.Items.AddRange(Languages);
        language.SelectedItem = Languages[0];
        format.Items.AddRange(["Compressed radix (NGTR v4)", "Compact (NGTR v3)", "Legacy (NGTR v2)"]);
        smoothing.Items.AddRange(["Modified Kneser-Ney", "Counts"]);
        format.SelectedIndex = 0; smoothing.SelectedIndex = 1;
        format.SelectedIndexChanged += (_, _) => UpdateFormat(selectDefault: true);

        var shell = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new(20), ColumnCount = 1, RowCount = 2 };
        shell.RowStyles.Add(new(SizeType.Percent, 100)); shell.RowStyles.Add(new(SizeType.AutoSize));
        var settingsPanel = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        var layout = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 3, Padding = new(0, 0, 16, 8) };
        layout.ColumnStyles.Add(new(SizeType.AutoSize));
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new(SizeType.AutoSize));
        settingsPanel.Controls.Add(layout); shell.Controls.Add(settingsPanel, 0, 0);
        var footer = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 3 };
        footer.ColumnStyles.Add(new(SizeType.Percent, 100)); footer.ColumnStyles.Add(new(SizeType.AutoSize)); footer.ColumnStyles.Add(new(SizeType.AutoSize));
        shell.Controls.Add(footer, 0, 1); Controls.Add(shell);

        AddRow("Corpus files or folders (one per line)", directories, 0, add);
        AddRow("Corpus search", includeSubdirectories, 1);
        AddRow("Trie output file", output, 2, browse);
        var languageNotice = new Label { AutoSize = true, MaximumSize = new Size(420, 0), Margin = new Padding(3, 6, 3, 0) };
        var languageControls = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        languageControls.Controls.AddRange([language, languageNotice]);
        void DescribeLanguage() => languageNotice.Text = SelectedLanguage is "ar" or "hi" or "zh" or "bn" or "id" or "ur" or "ru"
            ? SelectedLanguage == "zh" ? "NFC Han character n-grams; no bundled vocabulary filter." : "NFC Unicode words and combining marks; no bundled vocabulary filter."
            : "Uses this language's bundled vocabulary filter.";
        language.SelectedIndexChanged += (_, _) => DescribeLanguage(); DescribeLanguage();
        AddRow("Corpus language", languageControls, 3);
        AddRow("Worker threads", workers, 4);
        AddRow("RAM budget (MiB)", memory, 5);
        AddRow("Model format", format, 6);
        AddRow("Prediction scoring", smoothing, 7);
        AddRow("Model size budget (bytes)", bytes, 8);
        AddRow("Additional sizes (bytes)", profiles, 9);

        var limits = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        limits.Controls.AddRange([new Label { Text = "Unigrams", AutoSize = true }, unigrams,
            new Label { Text = "Bigrams", AutoSize = true }, bigrams, new Label { Text = "Trigrams", AutoSize = true }, trigrams]);
        AddRow("N-gram counts", limits, 10);
        useCache.Text = "Reuse counts and trained models";
        AddRow("Cache", useCache, 11);
        AddRow("Low-memory mode", lowMemory, 12);

        backend.Text = GenerationBackend.Describe();
        backend.MaximumSize = new Size(550, 0);
        AddRow("Generation engine", backend, 13);
        AddRow("Size budgets", new Label { AutoSize = true, MaximumSize = new Size(550, 0),
            Text = "Radix budgets include the lookup index. Compatibility formats keep their original trie-only budget. Additional sizes reuse the same training run." }, 14);
        AddRow("Scoring choices", new Label { AutoSize = true, MaximumSize = new Size(550, 0),
            Text = "Counts is the default. Modified Kneser-Ney can improve next-word context, but costs model space and training time. Benchmark your language and size profile." }, 15);
        var actions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        actions.Controls.AddRange([generate, stop]);
        AddWide(actions, 0, SizeType.AutoSize);
        AddWide(bar, 1, SizeType.AutoSize);
        AddWide(timing, 2, SizeType.AutoSize);
        status.MinimumSize = new Size(0, 75);
        AddWide(status, 3, SizeType.AutoSize);

        add.Click += (_, _) => AddFolder();
        browse.Click += (_, _) => ChooseOutput();
        generate.Click += Generate;
        stop.Click += (_, _) => { stop.Enabled = false; current?.Cancel(); };
        timer.Tick += (_, _) => UpdateProgress();
        FormClosing += (_, e) =>
        {
            if (current is null) { SaveSettings(); return; }
            e.Cancel = true;
            current.Cancel();
        };
        FormClosed += (_, _) => timer.Dispose();
        AcceptButton = generate;
        LoadSettings();

        void AddRow(string label, Control field, int row, Control? button = null)
        {
            layout.RowStyles.Add(new(SizeType.AutoSize));
            layout.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left }, 0, row);
            layout.Controls.Add(field, 1, row);
            if (button is not null) layout.Controls.Add(button, 2, row);
        }

        void AddWide(Control control, int row, SizeType size)
        {
            footer.RowStyles.Add(new(size, size == SizeType.Percent ? 100 : 0));
            footer.Controls.Add(control, 0, row);
            footer.SetColumnSpan(control, 3);
        }
    }

    private void AddFolder()
    {
        using var dialog = new FolderBrowserDialog { Description = "Add a corpus folder", UseDescriptionForTitle = true };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        var lines = directories.Lines.Where(line => !string.IsNullOrWhiteSpace(line)).ToList();
        if (lines.Contains(dialog.SelectedPath, StringComparer.OrdinalIgnoreCase)) return;
        lines.Add(dialog.SelectedPath);
        directories.Lines = [.. lines];
    }

    private void ChooseOutput()
    {
        using var dialog = new SaveFileDialog { Filter = "N-gram trie (*.ngtr)|*.ngtr", FileName = output.Text, OverwritePrompt = false };
        if (dialog.ShowDialog(this) == DialogResult.OK) output.Text = dialog.FileName;
    }

    private async void Generate(object? sender, EventArgs e)
    {
        var folders = directories.Lines.Select(line => line.Trim().Trim('"')).Where(line => line.Length != 0)
            .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        if (folders.Length == 0) { status.Text = "Add at least one corpus file or folder."; return; }
        var missing = folders.FirstOrDefault(path => !Directory.Exists(path) && !File.Exists(path));
        if (missing is not null) { status.Text = $"Corpus input not found: {missing}"; return; }

        string destination;
        try { destination = Path.GetFullPath(output.Text.Trim()); }
        catch (Exception error) when (error is ArgumentException or NotSupportedException)
        { status.Text = "Enter a valid trie output file."; return; }
        if (Directory.Exists(destination)) { status.Text = "Choose a file for the trie output."; return; }
        string primaryText = bytes.Text.Trim();
        ulong primarySize = 0;
        ulong minimumSize = format.SelectedIndex == 0 ? 64UL : format.SelectedIndex == 1 ? 20UL : 40UL;
        if (primaryText.Length > 0 && (!ulong.TryParse(primaryText, NumberStyles.None, CultureInfo.InvariantCulture, out primarySize) || primarySize < minimumSize))
        { status.Text = $"Model size budget must be at least {minimumSize} bytes for this format, or blank for unlimited."; return; }
        var sizes = new List<ulong>();
        foreach (string item in format.SelectedIndex == 0 ? profiles.Text.Split(',', StringSplitOptions.TrimEntries) : [])
        {
            if (string.IsNullOrWhiteSpace(profiles.Text)) break;
            if (!ulong.TryParse(item, NumberStyles.None, CultureInfo.InvariantCulture, out ulong size) || size < 64)
            { status.Text = "Additional radix sizes must be whole numbers of at least 64 bytes, including the index, separated by commas."; return; }
            if (!sizes.Contains(size)) sizes.Add(size);
        }
        var outputs = new List<string> { destination };
        outputs.AddRange(sizes.Where(size => size != primarySize).Select(size => Path.Combine(Path.GetDirectoryName(destination)!,
            Path.GetFileNameWithoutExtension(destination) + "-" + size.ToString(CultureInfo.InvariantCulture) + Path.GetExtension(destination))));
        if (format.SelectedIndex == 0) outputs.AddRange(outputs.Select(path => Path.ChangeExtension(path, ".ngix")).ToArray());
        var existing = outputs.Where(File.Exists).ToArray();
        if (existing.Length > 0 && MessageBox.Show(this, "Replace these existing model files?" + Environment.NewLine + string.Join(Environment.NewLine, existing), Text,
            MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;

        var args = new List<string>
        {
            "--trie-output", destination, "--workers", workers.Value.ToString(CultureInfo.InvariantCulture),
            "--language", SelectedLanguage,
            "--format", format.SelectedIndex == 0 ? "radix" : format.SelectedIndex == 1 ? "compact" : "legacy",
            "--smoothing", smoothing.SelectedIndex == 0 ? "modified-kn" : "counts",
            "--memory-mib", memory.Value.ToString(CultureInfo.InvariantCulture),
        };
        AddOption("--trie-size", bytes);
        AddOption("--unigrams", unigrams);
        AddOption("--bigrams", bigrams);
        AddOption("--trigrams", trigrams);
        if (!includeSubdirectories.Checked) args.Add("--no-subdirectories");
        if (!useCache.Checked) args.Add("--no-cache");
        if (lowMemory.Checked) args.Add("--low-memory");
        if (sizes.Count > 0) args.AddRange(["--profile-sizes", string.Join(",", sizes.Select(size => size.ToString(CultureInfo.InvariantCulture)))]);
        args.Add("--");
        args.AddRange(folders);

        using var cancellation = new CancellationTokenSource();
        current = cancellation;
        SetRunning(true);
        status.Clear();
        backend.Text = GenerationBackend.Describe();
        SaveSettings();
        progress = new("Finding corpus files", 0);
        clock.Restart();
        timer.Start();
        UpdateProgress();
        var updates = new Progress<GenerationProgress>(update =>
        {
            if (current != cancellation) return;
            progress = update;
            UpdateProgress();
        });

        try
        {
            GenerationBackend.Metrics? details = null;
            var result = await GenerationBackend.GenerateAsync([.. args], ((IProgress<GenerationProgress>)updates).Report,
                cancellation.Token, value => details = value);
            progress = new("Complete", 100);
            UpdateProgress();
            status.Text = $"Saved {result.TrieBytes:N0} bytes to {destination}{Environment.NewLine}" +
                          $"{result.Unigrams:N0} unigrams · {result.Bigrams:N0} bigrams · {result.Trigrams:N0} trigrams";
            if (details is not null)
            {
                status.AppendText($"{Environment.NewLine}Lookup index: {details.IndexBytes:N0} bytes · Model + index: {result.TrieBytes + details.IndexBytes:N0} bytes");
                if (details.EffectiveWorkers > 0) status.AppendText($" · Workers used: {details.EffectiveWorkers}");
                if (details.TrainingCacheHit) status.AppendText(" · Reused trained model");
                if (details.ProfilePaths.Count > 0) status.AppendText($"{Environment.NewLine}Created {details.ProfilePaths.Count:N0} model files from the same training run.");
            }
            TrieCreated?.Invoke(destination, SelectedLanguage);
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { status.Text = "Generation stopped."; }
        catch (Exception error)
        {
            status.Text = error is AggregateException aggregate ? aggregate.Flatten().InnerExceptions[0].Message : error.Message;
        }
        finally
        {
            timer.Stop();
            clock.Stop();
            current = null;
            SetRunning(false);
        }

        void AddOption(string flag, TextBox field)
        {
            if (string.IsNullOrWhiteSpace(field.Text)) return;
            args.Add(flag);
            args.Add(field.Text.Trim());
        }
    }

    private void UpdateProgress()
    {
        bar.Value = progress.Percent;
        string eta = progress.Percent == 0 ? "Calculating…" :
            FormatTime(TimeSpan.FromSeconds(clock.Elapsed.TotalSeconds * (100 - progress.Percent) / progress.Percent));
        timing.Text = $"{progress.Stage} · {progress.Percent}% · Estimated time remaining {eta}";
    }

    private static string FormatTime(TimeSpan time) => $"{(long)time.TotalHours:D2}:{time.Minutes:D2}:{time.Seconds:D2}";

    private string SelectedLanguage => ((LanguageOption)language.SelectedItem!).Code;

    private void LoadSettings()
    {
        try
        {
            if (settings.Load() is not { } saved) return;
            directories.Lines = saved.Directories ?? [];
            includeSubdirectories.Checked = saved.IncludeSubdirectories;
            output.Text = saved.Output;
            language.SelectedItem = Languages.FirstOrDefault(option => option.Code == saved.Language) ?? Languages[0];
            bytes.Text = saved.MaximumBytes;
            unigrams.Text = saved.Unigrams;
            bigrams.Text = saved.Bigrams;
            trigrams.Text = saved.Trigrams;
            workers.Value = Math.Clamp(saved.Workers, workers.Minimum, workers.Maximum);
            useCache.Checked = saved.Cache;
            lowMemory.Checked = saved.LowMemory;
            format.SelectedIndex = saved.Format == "radix" ? 0 : saved.Format == "compact" ? 1 : saved.Format == "legacy" ? 2 : saved.Compact ? 1 : 2;
            smoothing.SelectedIndex = saved.Smoothing == "modified-kn" && format.SelectedIndex == 0 ? 0 : 1;
            memory.Value = Math.Clamp(saved.MemoryMiB, memory.Minimum, memory.Maximum);
            profiles.Text = saved.ProfileSizes;
            UpdateFormat();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        { status.Text = $"Could not load settings: {error.Message}"; }
    }

    private void SaveSettings()
    {
        try
        {
            settings.Save(new(directories.Lines, output.Text, bytes.Text, unigrams.Text, bigrams.Text, trigrams.Text,
                (int)workers.Value, useCache.Checked, lowMemory.Checked, format.SelectedIndex != 2, SelectedLanguage,
                includeSubdirectories.Checked, format.SelectedIndex == 0 ? "radix" : format.SelectedIndex == 1 ? "compact" : "legacy",
                smoothing.SelectedIndex == 0 ? "modified-kn" : "counts", (int)memory.Value, profiles.Text));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { status.Text = $"Could not save settings: {error.Message}"; }
    }

    private void SetRunning(bool running)
    {
        foreach (var control in new Control[] { directories, includeSubdirectories, output, language, bytes, unigrams,
            bigrams, trigrams, workers, useCache, lowMemory, format, smoothing, memory, profiles, add, browse, generate })
            control.Enabled = !running;
        stop.Enabled = running;
        UpdateFormat();
    }

    private void UpdateFormat(bool selectDefault = false)
    {
        bool radix = format.SelectedIndex == 0;
        if (!radix) smoothing.SelectedIndex = 1;
        else if (selectDefault) smoothing.SelectedIndex = 1;
        smoothing.Enabled = current is null && radix;
        profiles.Enabled = current is null && radix;
    }
}
