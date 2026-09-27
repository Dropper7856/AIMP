namespace AIMP.Launcher;

internal sealed class SettingsForm : Form
{
    private readonly LauncherSettingsStore _store;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly TextBox _gameDirectory = new() { Dock = DockStyle.Fill };
    private readonly TextBox _playerName = new() { Dock = DockStyle.Fill };
    private readonly TextBox _hostAddress = new() { Dock = DockStyle.Fill };
    private readonly NumericUpDown _port = CreatePortEditor();
    private readonly CheckBox _worldSyncEnabled = new()
    {
        Text = "Enable WorldSync (applies next game session)",
        AutoSize = true,
        Checked = true
    };
    private readonly CheckBox _worldSync3EmptyCallbackEnabled = new()
    {
        Text = "WS3: ProbeDirect callback compatibility test only (next game session)",
        AutoSize = true,
        Checked = false
    };
    private readonly Label _validation = new() { AutoSize = true, ForeColor = Color.DimGray };
    private readonly Button _saveButton = new() { Text = "Save", AutoSize = true };
    private Guid _playerId;

    internal SettingsForm(LauncherSettingsStore store, LauncherDiagnostics diagnostics)
    {
        _store = store ?? throw new ArgumentNullException(nameof(store));
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
        ConfigureWindow();
        Controls.Add(BuildInterface());
        SubscribeEvents();
        LoadCurrentSettings();
    }

    private static NumericUpDown CreatePortEditor() => new()
    {
        Dock = DockStyle.Left,
        Minimum = 1,
        Maximum = 65535,
        Value = AIMP.Protocol.ProtocolConstants.DefaultGamePort,
        Width = 120
    };

    private void ConfigureWindow()
    {
        Text = "Settings";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ClientSize = new Size(650, 380);
    }

    private Control BuildInterface()
    {
        var root = CreateRoot();
        AddField(root, 0, "Alien: Isolation game directory", BuildPathEditor());
        AddField(root, 1, "Player name", _playerName);
        AddField(root, 2, "Server IP or host name", _hostAddress);
        AddField(root, 3, "Server port", _port);
        AddField(root, 4, "WorldSync", BuildWorldSyncEditor());
        root.Controls.Add(_validation, 0, 10);
        root.SetColumnSpan(_validation, 2);
        root.Controls.Add(BuildButtons(), 0, 11);
        root.SetColumnSpan(root.GetControlFromPosition(0, 11)!, 2);
        return root;
    }

    private static TableLayoutPanel CreateRoot()
    {
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 2, RowCount = 12
        };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (var row = 0; row < 10; row++) root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        return root;
    }

    private static void AddField(TableLayoutPanel root, int index, string label, Control editor)
    {
        var row = index * 2;
        root.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left }, 0, row);
        root.Controls.Add(editor, 1, row);
        editor.Margin = new Padding(3, 3, 3, 10);
    }

    private Control BuildWorldSyncEditor()
    {
        var panel = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = true,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false
        };
        panel.Controls.Add(_worldSyncEnabled);
        panel.Controls.Add(_worldSync3EmptyCallbackEnabled);
        return panel;
    }

    private Control BuildPathEditor()
    {
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        var browse = new Button { Text = "Browse...", AutoSize = true };
        browse.Click += OnBrowseClick;
        panel.Controls.Add(_gameDirectory, 0, 0);
        panel.Controls.Add(browse, 1, 0);
        return panel;
    }

    private Control BuildButtons()
    {
        var panel = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill, AutoSize = true, FlowDirection = FlowDirection.RightToLeft
        };
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
        panel.Controls.Add(cancel);
        panel.Controls.Add(_saveButton);
        AcceptButton = _saveButton;
        CancelButton = cancel;
        return panel;
    }

    private void SubscribeEvents()
    {
        _gameDirectory.TextChanged += OnSettingsChanged;
        _playerName.TextChanged += OnSettingsChanged;
        _hostAddress.TextChanged += OnSettingsChanged;
        _port.ValueChanged += OnSettingsChanged;
        _worldSyncEnabled.CheckedChanged += OnSettingsChanged;
        _worldSync3EmptyCallbackEnabled.CheckedChanged += OnSettingsChanged;
        _saveButton.Click += OnSaveClick;
    }

    private void LoadCurrentSettings()
    {
        try { ApplySettings(_store.Load()); }
        catch (Exception exception) { ShowLoadFailure(exception); }
        UpdateValidation();
    }

    private void ApplySettings(LauncherSettings settings)
    {
        _gameDirectory.Text = settings.GameDirectory;
        _playerName.Text = settings.PlayerName;
        _hostAddress.Text = settings.HostAddress;
        _port.Value = settings.Port;
        _worldSyncEnabled.Checked = settings.WorldSyncEnabled;
        _worldSync3EmptyCallbackEnabled.Checked = settings.WorldSync3EmptyCallbackEnabled;
        _playerId = settings.PlayerId;
    }

    private void ShowLoadFailure(Exception exception)
    {
        _diagnostics.Log("Settings", $"Settings load failed: {exception.Message}");
        ApplySettings(LauncherSettings.Empty.Normalize());
    }

    private void OnBrowseClick(object? sender, EventArgs eventArgs)
    {
        using var dialog = CreateFolderDialog();
        if (dialog.ShowDialog(this) == DialogResult.OK) _gameDirectory.Text = dialog.SelectedPath;
    }

    private FolderBrowserDialog CreateFolderDialog() => new()
    {
        Description = "Select the folder that contains AI.exe",
        UseDescriptionForTitle = true,
        SelectedPath = Directory.Exists(_gameDirectory.Text) ? _gameDirectory.Text : string.Empty
    };

    private void OnSettingsChanged(object? sender, EventArgs eventArgs) => UpdateValidation();

    private void UpdateValidation()
    {
        var valid = TryReadSettings(out _, out var error);
        _validation.Text = valid ? "Game and multiplayer settings are valid." : error;
        _validation.ForeColor = valid ? Color.DarkGreen : Color.DarkOrange;
        _saveButton.Enabled = valid;
    }

    private bool TryReadSettings(out LauncherSettings settings, out string error)
    {
        settings = LauncherSettings.Empty;
        if (!GameInstallation.TryResolve(_gameDirectory.Text, out var paths, out error)) return false;
        if (!MultiplayerSettingsValidation.TryValidate(_playerName.Text, _hostAddress.Text,
                _port.Value, out var name, out var host, out var port, out error)) return false;
        settings = new LauncherSettings(paths.GameDirectory, name, host, port,
            _playerId == Guid.Empty ? Guid.NewGuid() : _playerId,
            _worldSyncEnabled.Checked,
            _worldSync3EmptyCallbackEnabled.Checked);
        return true;
    }

    private void OnSaveClick(object? sender, EventArgs eventArgs)
    {
        if (!TryReadSettings(out var settings, out var error))
        { ShowValidationError(error); return; }
        try { SaveSettings(settings); }
        catch (Exception exception) { ShowSaveFailure(exception); }
    }

    private void SaveSettings(LauncherSettings settings)
    {
        _store.Save(settings);
        WorldSync3LauncherOptions.ApplyProcessEnvironment(settings);
        _diagnostics.Log("Settings",
            $"Settings saved for player {settings.PlayerName}; WorldSync={(settings.WorldSyncEnabled ? "enabled" : "disabled")}; " +
            $"WS3 ProbeDirect callback test={(settings.WorldSync3EmptyCallbackEnabled ? "enabled" : "disabled")} for the next game session.");
        DialogResult = DialogResult.OK;
        Close();
    }

    private void ShowSaveFailure(Exception exception)
    {
        _diagnostics.Log("Settings", $"Settings save failed: {exception.Message}");
        ShowValidationError($"The settings could not be saved: {exception.Message}");
    }

    private void ShowValidationError(string message) => MessageBox.Show(
        this, message, "Settings", MessageBoxButtons.OK, MessageBoxIcon.Warning);
}
