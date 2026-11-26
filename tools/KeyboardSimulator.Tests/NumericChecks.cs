using System.Reflection;
using KeyboardSimulator;
using NgramTrie.Reader;

namespace KeyboardLanguageChecks;

internal static class NumericChecks
{
    private static void Check(bool value, string message)
    {
        if (!value) throw new InvalidOperationException(message);
    }

    public static void Run(string profiles)
    {
        using var preview = new KeyboardPreview { Size = new(1024, 720) };
        preview.SetKeyboard(KeyboardDefinition.Load(Path.Combine(profiles, "en-US-qwerty.json")));
        preview.NumericInput = true;
        var rows = NumericKeyboard.Definition.Layouts["numeric"].Rows;
        Check(rows.Count == 4 && rows.All(row => row.Keys.Count == 3), "Numeric keypad is not 4 by 3");
        Check(rows.SelectMany(row => row.Keys).Select(key => key.Label).SequenceEqual("123456789.0⌫".Select(c => c.ToString())),
            "Numeric keypad differs from the device layout");
        foreach (char digit in "0123456789") preview.PressKey("digit-" + digit);
        preview.PressKey("decimal"); preview.PressKey("digit-5"); preview.PressKey("decimal");
        Check(preview.TextValue == "0123456789.5", "On-screen numeric input accepted an extra decimal");
        preview.PressKey("backspace");
        Check(preview.TextValue == "0123456789.", "Numeric delete failed");
        preview.TextValue = "-1a2.3.4e5 العربية १२😀";
        Check(preview.TextValue == "12.345", "Numeric assignment accepted signs, letters, Unicode digits or extra dots");
        preview.EditorControl.Select(1, 0); preview.TypeText(".");
        Check(preview.TextValue == "12.345" && preview.EditorControl.SelectionStart == 1,
            "Inserting an extra decimal moved the existing decimal or caret");
        preview.EditorControl.Select(2, 1); preview.TypeText(".");
        Check(preview.TextValue == "12.345", "Replacing the selected decimal was rejected");
        preview.EditorControl.Select(0, 2); preview.TypeText("abc");
        Check(preview.TextValue == "12.345" && preview.EditorControl.SelectionLength == 2,
            "Rejected input deleted or deselected numeric text");
        preview.EditorControl.SelectedText = "-9x.8";
        Check(preview.TextValue == "98.345", "Paste-equivalent replacement displaced the existing decimal");
        preview.EditorControl.Select(0, preview.TextValue.Length);
        preview.EditorControl.SelectedText = "-7.8.9e+2";
        Check(preview.TextValue == "7.892", "Selected-text paste did not filter the complete replacement");
        preview.EditorControl.Select(1, 2); preview.PressKey("backspace");
        Check(preview.TextValue == "792", "Numeric selection deletion failed");
        preview.TextValue = "";
        var keyPress = typeof(PreviewTextBox).GetMethod("OnKeyPress", BindingFlags.Instance | BindingFlags.NonPublic)!;
        foreach (char character in "a-1.2.3\r")
        {
            var key = new KeyPressEventArgs(character);
            keyPress.Invoke(preview.EditorControl, [key]);
            Check(key.Handled, "Numeric physical input bypassed filtering");
        }
        Check(preview.TextValue == "1.23", "Physical numeric input differs from on-screen policy");
        preview.TextValue = "."; preview.EditorControl.Select(0, 0); preview.TypeText("5");
        Check(preview.TextValue == "5.", "Numeric caret insertion did not retain a single decimal");
        preview.NumericInput = false;
        preview.TextValue = ""; preview.PressKey("a");
        Check(preview.TextValue == "a", "Turning numeric off did not restore the selected text layout");

        using var form = new SimulatorForm(profiles);
        var controls = Descendants(form).ToArray();
        var option = controls.OfType<CheckBox>().Single(control => control.AccessibleName == "Numeric keyboard");
        var actualPreview = controls.OfType<KeyboardPreview>().Single();
        form.UseLanguage("zh"); actualPreview.PressKey("n"); actualPreview.PressKey("i");
        option.Checked = true;
        Check(actualPreview.NumericInput && actualPreview.CompositionText == "", "Numeric toggle retained Chinese composition");
        Check(!controls.OfType<ComboBox>().Single(control => control.AccessibleName == "Keyboard").Enabled,
            "Language selector remained active during numeric simulation");
        actualPreview.PressKey("digit-4");
        Check(actualPreview.TextValue == "4", "Numeric toolbar option did not activate the keypad");
        option.Checked = false; actualPreview.TextValue = "";
        actualPreview.PressKey("n"); actualPreview.PressKey("i");
        Check(actualPreview.CompositionText == "ni", "Numeric toggle lost the selected Chinese layout");
        Console.WriteLine("Numeric keypad, filtering, selection edits, physical typing and toolbar language restoration passed.");
    }

    public static void WithTrie(TrieModel trie, string profiles)
    {
        // Prediction resumes on the WinForms message loop, just as it does in
        // the simulator. Keep the host off-screen while asynchronous work runs.
        using var host = new SimulatorForm(profiles) { ShowInTaskbar = false,
            StartPosition = FormStartPosition.Manual, Location = new(-32000, -32000) };
        var preview = Descendants(host).OfType<KeyboardPreview>().Single();
        Exception? failure = null;
        host.Shown += async (_, _) =>
        {
            try
            {
                foreach (int width in new[] { 1140, host.MinimumSize.Width })
                {
                    host.Width = width;
                    foreach (var control in Descendants(host).Where(control =>
                        control is ComboBox or NumericUpDown or CheckBox || control.Text == "Load trie…" || control.Text == "View trie…"))
                    {
                        var bounds = host.RectangleToClient(control.RectangleToScreen(control.ClientRectangle));
                        Check(control.Width > 0 && host.ClientRectangle.Contains(bounds),
                            "Simulator toolbar extends outside the window at width " + width);
                    }
                }
                host.Width = 1140;
                await CheckTrie(preview, trie, profiles);
            }
            catch (Exception error) { failure = error; }
            finally { host.Close(); }
        };
        Application.Run(host);
        if (failure is not null) throw new InvalidOperationException("Numeric trie simulation failed", failure);
    }

    private static async Task CheckTrie(KeyboardPreview preview, TrieModel trie, string profiles)
    {
        preview.SetKeyboard(KeyboardDefinition.Load(Path.Combine(profiles, "en-US-qwerty.json")));
        preview.SetTrie(trie);
        preview.TextValue = "al"; preview.EditorControl.SelectionStart = 2;
        preview.RefreshSuggestionsNow();
        preview.NumericInput = true;
        preview.TypeText("12.3"); preview.RefreshSuggestionsNow();
        await Task.Delay(100);
        Check(preview.EditorControl.Height > preview.EditorControl.Font.Height * 2,
            "Numeric input shrank the preview's clickable editing area to one line");
        Check(preview.TextValue == "12.3" && preview.CandidateWords.All(string.IsNullOrEmpty),
            "A pending trie result, suggestion or autocorrection changed numeric input");
        preview.NumericInput = false;
        preview.TextValue = "al"; preview.EditorControl.SelectionStart = 2;
        preview.RefreshSuggestionsNow();
        long deadline = Environment.TickCount64 + 5000;
        while (!preview.CandidateWords.Any(word => word.StartsWith("al", StringComparison.Ordinal)) &&
            Environment.TickCount64 < deadline) await Task.Delay(5);
        Check(preview.CandidateWords.Any(word => word.StartsWith("al", StringComparison.Ordinal)),
            "Turning numeric off did not resume the retained trie");
        Console.WriteLine("Numeric mode suppresses pending predictions and restores the loaded trie.");
    }

    private static IEnumerable<Control> Descendants(Control parent)
    {
        foreach (Control child in parent.Controls)
        {
            yield return child;
            foreach (var nested in Descendants(child)) yield return nested;
        }
    }

}
