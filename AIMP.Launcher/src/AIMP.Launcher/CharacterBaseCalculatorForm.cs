namespace AIMP.Launcher;

internal sealed class CharacterBaseCalculatorForm : Form
{
    private readonly TextBox _ecx = new() { Text = "0x00000000", Dock = DockStyle.Top };
    private readonly TextBox _result = new() { ReadOnly = true, Dock = DockStyle.Top };
    private readonly Label _status = new() { AutoSize = true };

    internal CharacterBaseCalculatorForm()
    {
        ConfigureWindow();
        Controls.Add(BuildInterface());
    }

    private void ConfigureWindow()
    {
        Text = "CharacterBase Calculator";
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(520, 270);
        MinimumSize = new Size(500, 260);
        ShowInTaskbar = false;
        Icon = SystemIcons.Application;
    }

    private Control BuildInterface()
    {
        var root = CreateRoot();
        var description = Description();
        root.Controls.Add(description, 0, 0);
        root.SetColumnSpan(description, 2);
        AddField(root, 1, "ECX address", _ecx);
        AddField(root, 2, "CharacterBase", _result);
        root.Controls.Add(_status, 0, 3);
        root.SetColumnSpan(_status, 2);
        root.Controls.Add(BuildButtons(), 0, 4);
        root.SetColumnSpan(root.GetControlFromPosition(0, 4)!, 2);
        return root;
    }

    private static TableLayoutPanel CreateRoot() => new()
    {
        Dock = DockStyle.Fill,
        Padding = new Padding(12),
        ColumnCount = 2,
        RowCount = 5,
        ColumnStyles = { new ColumnStyle(SizeType.Absolute, 140),
            new ColumnStyle(SizeType.Percent, 100) }
    };

    private static Label Description() => new()
    {
        AutoSize = true,
        MaximumSize = new Size(480, 0),
        Text = "For bp mod.main()+27D370: CharacterBase = ECX - 0x3570. "
            + "Do not use an ECX captured at another breakpoint."
    };

    private static void AddField(TableLayoutPanel root, int row, string text, Control editor)
    {
        var label = new Label { Text = text, AutoSize = true, Anchor = AnchorStyles.Left };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.Controls.Add(label, 0, row);
        root.Controls.Add(editor, 1, row);
    }

    private Control BuildButtons()
    {
        var calculate = new Button { Text = "Calculate", AutoSize = true };
        var copy = new Button { Text = "Copy", AutoSize = true };
        var close = new Button { Text = "Close", AutoSize = true, DialogResult = DialogResult.Cancel };
        calculate.Click += OnCalculate;
        copy.Click += OnCopy;
        var panel = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        panel.Controls.Add(calculate);
        panel.Controls.Add(copy);
        panel.Controls.Add(close);
        CancelButton = close;
        return panel;
    }

    private void OnCalculate(object? sender, EventArgs eventArgs)
    {
        if (!CharacterAddressCalculator.TryParseHex32(_ecx.Text, out var ecx))
        {
            ShowError("Enter a valid 32-bit hexadecimal ECX address.");
            return;
        }
        try { ShowResult(CharacterAddressCalculator.Calculate(ecx)); }
        catch (InvalidOperationException exception) { ShowError(exception.Message); }
    }

    private void ShowResult(uint characterBase)
    {
        _result.Text = CharacterAddressCalculator.Format(characterBase);
        _status.Text = (characterBase & 0x0FU) == 0U
            ? "CharacterBase is 16-byte aligned." : "Warning: result is not 16-byte aligned.";
        _status.ForeColor = (characterBase & 0x0FU) == 0U ? Color.DarkGreen : Color.DarkOrange;
    }

    private void ShowError(string message)
    {
        _result.Clear();
        _status.Text = message;
        _status.ForeColor = Color.DarkRed;
    }

    private void OnCopy(object? sender, EventArgs eventArgs)
    {
        if (_result.Text.Length == 0) return;
        Clipboard.SetText(_result.Text);
        _status.Text = "CharacterBase copied to the clipboard.";
        _status.ForeColor = Color.DarkGreen;
    }
}
