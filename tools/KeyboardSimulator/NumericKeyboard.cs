using System.Runtime.InteropServices;
using System.Diagnostics.CodeAnalysis;
using System.Text;

namespace KeyboardSimulator;

internal static class NumericKeyboard
{
    public static KeyboardDefinition Definition { get; } = new()
    {
        Version = 1, Id = "numeric", InitialLayout = "numeric", Width = 3,
        Layouts = new Dictionary<string, KeyboardLayout>(StringComparer.Ordinal)
        {
            ["numeric"] = new()
            {
                Rows =
                [
                    new() { Keys = [Digit('1'), Digit('2'), Digit('3')] },
                    new() { Keys = [Digit('4'), Digit('5'), Digit('6')] },
                    new() { Keys = [Digit('7'), Digit('8'), Digit('9')] },
                    new() { Keys = [new() { Id = "decimal", Label = ".", Action = new() { Kind = KeyboardActionKind.Insert, Text = "." } },
                        Digit('0'), new() { Id = "backspace", Label = "⌫", Action = new() { Kind = KeyboardActionKind.Backspace } }] },
                ],
            },
        },
    };

    private static KeyboardKey Digit(char digit) => new()
    {
        Id = "digit-" + digit, Label = digit.ToString(),
        Action = new() { Kind = KeyboardActionKind.Insert, Text = digit.ToString() },
    };
}

/// <summary>Applies the runtime's ASCII digits and single decimal point input policy.</summary>
internal sealed class PreviewTextBox : TextBox
{
    private const int WmPaste = 0x0302;
    private bool numericOnly;
    private bool normalizing;

    public bool NumericOnly
    {
        get => numericOnly;
        set
        {
            if (numericOnly == value) return;
            numericOnly = value;
            if (value && NormalizeText()) base.OnTextChanged(EventArgs.Empty);
        }
    }

    [AllowNull]
    public override string SelectedText
    {
        get => base.SelectedText;
        set
        {
            // Empty replacement is deletion, including the on-screen backspace key.
            if (numericOnly && !string.IsNullOrEmpty(value)) InsertNumericText(value);
            else base.SelectedText = value ?? "";
        }
    }

    public void InsertNumericText(string text)
    {
        if (!numericOnly) { base.SelectedText = text; return; }
        int start = SelectionStart, end = start + SelectionLength;
        string current = Text;
        bool decimalPoint = current.AsSpan(0, start).Contains('.') || current.AsSpan(end).Contains('.');
        var accepted = new StringBuilder(text.Length);
        foreach (char character in text)
        {
            if (character is >= '0' and <= '9') accepted.Append(character);
            else if (character == '.' && !decimalPoint) { accepted.Append(character); decimalPoint = true; }
        }
        // Rejected input must not delete the selected text or move its selection.
        if (accepted.Length != 0) base.SelectedText = accepted.ToString();
    }

    protected override void OnKeyPress(KeyPressEventArgs e)
    {
        if (numericOnly && !e.Handled && (!char.IsControl(e.KeyChar) || e.KeyChar is '\r' or '\n'))
        {
            InsertNumericText(e.KeyChar.ToString());
            e.Handled = true;
        }
        base.OnKeyPress(e);
    }

    protected override void WndProc(ref Message m)
    {
        if (numericOnly && m.Msg == WmPaste)
        {
            // Paste uses the same selection-aware filtering as on-screen input.
            try { InsertNumericText(Clipboard.GetText()); }
            catch (ExternalException) { } // Another process may temporarily own the clipboard.
            return;
        }
        base.WndProc(ref m);
    }

    protected override void OnTextChanged(EventArgs e)
    {
        if (normalizing) return;
        if (numericOnly) NormalizeText();
        base.OnTextChanged(e);
    }

    private bool NormalizeText()
    {
        string original = Text;
        int start = SelectionStart, end = start + SelectionLength;
        int mappedStart = 0, mappedEnd = 0;
        bool decimalPoint = false;
        var accepted = new StringBuilder(original.Length);
        for (int index = 0; index < original.Length; index++)
        {
            char character = original[index];
            bool keep = character is >= '0' and <= '9' || character == '.' && !decimalPoint;
            if (!keep) continue;
            if (character == '.') decimalPoint = true;
            accepted.Append(character);
            if (index < start) mappedStart++;
            if (index < end) mappedEnd++;
        }
        if (accepted.Length == original.Length) return false;
        normalizing = true;
        try
        {
            Text = accepted.ToString();
            Select(mappedStart, mappedEnd - mappedStart);
        }
        finally { normalizing = false; }
        return true;
    }
}
