using System.Globalization;

namespace AIMP.Launcher;

internal sealed class ExecuteCommandForm : Form
{
    private readonly NativeRuntimeController _controller;
    private readonly MultiplayerSessionCoordinator _multiplayer;
    private readonly ComboBox _commandSelector = new();
    private readonly Label _description = new() { AutoSize = true };
    private readonly TableLayoutPanel _parameters = new();
    private readonly Label _state = new() { AutoSize = true };
    private readonly Label _result = new() { AutoSize = true };
    private readonly Button _execute = new() { Text = "Execute", AutoSize = true };
    private readonly TextBox _spawnCharacterBase = new()
        { ReadOnly = true, Dock = DockStyle.Fill, Text = "Not tracked" };
    private readonly Button _copySpawnCharacterBase = new()
        { Text = "Copy", AutoSize = true, Enabled = false };
    private readonly ToolTip _toolTip = new();
    private readonly Dictionary<string, Control> _editors = new(StringComparer.Ordinal);
    private readonly List<Button> _characterActionButtons = new();
    private int _executing;

    internal ExecuteCommandForm(
        NativeRuntimeController controller,
        MultiplayerSessionCoordinator multiplayer)
    {
        _controller = controller ?? throw new ArgumentNullException(nameof(controller));
        _multiplayer = multiplayer ?? throw new ArgumentNullException(nameof(multiplayer));
        _copySpawnCharacterBase.Click += OnCopySpawnCharacterBaseClick;
        ConfigureWindow();
        Controls.Add(BuildInterface());
        LoadCommands();
        SubscribeState();
        UpdateState(_controller.Snapshot);
    }

    private void SubscribeState()
    {
        _controller.StateChanged += OnStateChanged;
        _multiplayer.StateChanged += OnMultiplayerStateChanged;
        FormClosed += OnFormClosed;
    }

    private void ConfigureWindow()
    {
        Text = "Execute Command";
        StartPosition = FormStartPosition.CenterParent;
        MinimumSize = new Size(620, 500);
        ClientSize = new Size(720, 600);
        ShowInTaskbar = false;
        Icon = SystemIcons.Application;
    }

    private Control BuildInterface()
    {
        var root = CreateRootPanel();
        root.Controls.Add(CreateHeading("Command"), 0, 0);
        ConfigureSelector();
        root.Controls.Add(_commandSelector, 0, 1);
        ConfigureDescription();
        root.Controls.Add(_description, 0, 2);
        ConfigureParameters();
        root.Controls.Add(_parameters, 0, 3);
        ConfigureFeedback(root);
        root.Controls.Add(BuildButtons(), 0, 6);
        return root;
    }

    private static TableLayoutPanel CreateRootPanel()
    {
        var panel = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 1, RowCount = 7
        };
        for (var row = 0; row < 7; row++)
            panel.RowStyles.Add(new RowStyle(row == 3 ? SizeType.Percent : SizeType.AutoSize,
                row == 3 ? 100 : 0));
        return panel;
    }

    private static Label CreateHeading(string text)
    {
        var label = new Label { Text = text, AutoSize = true, Margin = new Padding(0, 0, 0, 5) };
        label.Font = new Font(label.Font, FontStyle.Bold);
        return label;
    }

    private void ConfigureSelector()
    {
        _commandSelector.Dock = DockStyle.Top;
        _commandSelector.DropDownStyle = ComboBoxStyle.DropDownList;
        _commandSelector.DisplayMember = nameof(GameCommandDefinition.Label);
        _commandSelector.SelectedIndexChanged += OnCommandSelectionChanged;
    }

    private void ConfigureDescription()
    {
        _description.MaximumSize = new Size(670, 0);
        _description.Margin = new Padding(0, 8, 0, 10);
    }

    private void ConfigureParameters()
    {
        _parameters.Dock = DockStyle.Fill;
        _parameters.AutoScroll = true;
        _parameters.ColumnCount = 2;
        _parameters.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 180));
        _parameters.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
    }

    private void ConfigureFeedback(TableLayoutPanel root)
    {
        _state.MaximumSize = new Size(670, 0);
        _state.Margin = new Padding(0, 10, 0, 4);
        _result.MaximumSize = new Size(670, 0);
        _result.Margin = new Padding(0, 4, 0, 10);
        root.Controls.Add(_state, 0, 4);
        root.Controls.Add(_result, 0, 5);
    }

    private Control BuildButtons()
    {
        var close = new Button
        {
            Text = "Close", AutoSize = true, DialogResult = DialogResult.Cancel
        };
        var panel = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill, AutoSize = true, FlowDirection = FlowDirection.RightToLeft
        };
        _execute.Click += OnExecuteClick;
        panel.Controls.Add(close);
        panel.Controls.Add(_execute);
        CancelButton = close;
        return panel;
    }

    private void LoadCommands()
    {
        _commandSelector.Items.Clear();
        foreach (var command in GameCommandCatalog.Commands) _commandSelector.Items.Add(command);
        if (_commandSelector.Items.Count > 0) _commandSelector.SelectedIndex = 0;
    }

    private void OnCommandSelectionChanged(object? sender, EventArgs eventArgs)
    {
        BuildParameterEditors();
        UpdateState(_controller.Snapshot);
    }

    private void BuildParameterEditors()
    {
        _parameters.SuspendLayout();
        _parameters.Controls.Clear();
        _parameters.RowStyles.Clear();
        _parameters.RowCount = 0;
        _editors.Clear();
        _characterActionButtons.Clear();
        if (_commandSelector.SelectedItem is GameCommandDefinition command)
        {
            _description.Text = command.Description;
            _execute.Text = "Execute";
            _execute.Visible = !IsCharacterActionTest(command);
            if (IsCharacterActionTest(command))
                AddCharacterActionTestControls(command);
            else
                AddCommandParameters(command);
        }
        else
        {
            _execute.Text = "Execute";
            _execute.Visible = true;
        }
        _parameters.ResumeLayout();
    }

    private void AddCommandParameters(GameCommandDefinition command)
    {
        foreach (var parameter in command.Parameters)
        {
            AddParameter(parameter);
        }
        if (IsDirectMove(command) || string.Equals(command.Name,
                AIMP.Protocol.GameCommandNames.CharacterAmmoRefill, StringComparison.Ordinal))
            ApplyTrackedCharacterAddress();
        if (IsSpawnCharacter(command)) AddSpawnCharacterTrackerRow();
    }



    private void AddCharacterActionTestControls(GameCommandDefinition command)
    {
        var characterParameter = command.Parameters.First(parameter =>
            string.Equals(parameter.Key, "characterAddress", StringComparison.Ordinal));
        AddParameter(characterParameter);
        ApplyTrackedCharacterAddress();

        var row = _parameters.RowCount++;
        var label = new Label
        {
            Text = "Actions", AutoSize = true, Anchor = AnchorStyles.Left,
            Margin = new Padding(3, 8, 8, 3)
        };
        var panel = new FlowLayoutPanel
        {
            Dock = DockStyle.Top, AutoSize = true, WrapContents = true,
            FlowDirection = FlowDirection.LeftToRight
        };
        AddCharacterActionButton(panel, "Aim START", "aim-start");
        AddCharacterActionButton(panel, "Aim STOP", "aim-stop");
        AddCharacterActionButton(panel, "Primary START", "primary-start");
        AddCharacterActionButton(panel, "Primary STOP", "primary-stop");
        var refill = new Button { Text = "Remplir les munitions de test", AutoSize = true };
        refill.Click += async (_, _) => await ExecuteCharacterAmmoRefillAsync();
        _characterActionButtons.Add(refill);
        panel.Controls.Add(refill);
        _parameters.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        _parameters.Controls.Add(label, 0, row);
        _parameters.Controls.Add(panel, 1, row);
    }

    private void AddCharacterActionButton(
        FlowLayoutPanel panel, string text, string action)
    {
        var button = new Button { Text = text, AutoSize = true };
        button.Click += async (_, _) => await ExecuteCharacterActionAsync(action);
        _characterActionButtons.Add(button);
        panel.Controls.Add(button);
    }

    private async Task ExecuteCharacterActionAsync(string action)
    {
        if (Interlocked.Exchange(ref _executing, 1) != 0) return;
        try
        {
            var command = SelectedCommand();
            if (!IsCharacterActionTest(command))
                throw new InvalidOperationException("Select Character Actions first.");
            if (!_editors.TryGetValue("characterAddress", out var editor))
                throw new InvalidOperationException("CharacterBase address is unavailable.");
            var parameter = command.Parameters.First(value =>
                string.Equals(value.Key, "characterAddress", StringComparison.Ordinal));
            var values = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["characterAddress"] = ReadEditorValue(editor, parameter),
                ["action"] = action
            };
            var normalized = GameCommandCatalog.ValidateAndNormalize(command, values);
            _result.Text = _controller.Snapshot.EngineThreadReady
                ? $"Submitting input intent: {action}"
                : $"Queueing input intent: {action}";
            var response = await DispatchCommandAsync(command, normalized);
            ShowCommandResult(response);
        }
        catch (Exception exception)
        {
            ShowCommandError(exception.Message);
        }
        finally
        {
            Interlocked.Exchange(ref _executing, 0);
            UpdateState(_controller.Snapshot);
        }
    }

    private async Task ExecuteCharacterAmmoRefillAsync()
    {
        if (Interlocked.Exchange(ref _executing, 1) != 0) return;
        try
        {
            if (!IsCharacterActionTest(SelectedCommand()))
                throw new InvalidOperationException("Select Character Actions first.");
            if (!_editors.TryGetValue("characterAddress", out var editor))
                throw new InvalidOperationException("CharacterBase address is unavailable.");
            var command = GameCommandCatalog.Commands.First(value => string.Equals(
                value.Name, AIMP.Protocol.GameCommandNames.CharacterAmmoRefill, StringComparison.Ordinal));
            var values = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["characterAddress"] = ReadEditorValue(editor, command.Parameters[0])
            };
            var normalized = GameCommandCatalog.ValidateAndNormalize(command, values);
            UpdateState(_controller.Snapshot);
            _result.Text = "Requesting manual test-ammo refill (Primary must be stopped).";
            var response = await DispatchCommandAsync(command, normalized);
            ShowCommandResult(response);
        }
        catch (Exception exception)
        {
            ShowCommandError(exception.Message);
        }
        finally
        {
            Interlocked.Exchange(ref _executing, 0);
            UpdateState(_controller.Snapshot);
        }
    }

    private void ApplyTrackedCharacterAddress()
    {
        var runtime = _controller.Snapshot;
        if (runtime.RemoteCharacterState != AIMP.Protocol.RemoteCharacterStateNames.Ready) return;
        if (!_editors.TryGetValue("characterAddress", out var editor)) return;
        if (editor is TextBox text)
            text.Text = $"0x{runtime.RemoteCharacterBase:X8}";
    }

    private void AddSpawnCharacterTrackerRow()
    {
        var row = _parameters.RowCount++;
        var label = new Label { Text = "Tracked CharacterBase", AutoSize = true,
            Anchor = AnchorStyles.Left, Margin = new Padding(3, 8, 8, 3) };
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 2 };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        panel.Controls.Add(_spawnCharacterBase, 0, 0);
        panel.Controls.Add(_copySpawnCharacterBase, 1, 0);
        _parameters.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        _parameters.Controls.Add(label, 0, row);
        _parameters.Controls.Add(panel, 1, row);
        UpdateSpawnCharacterAddress(_controller.Snapshot);
    }

    private void UpdateSpawnCharacterAddress(NativeRuntimeSnapshot value)
    {
        var ready = value.RemoteCharacterState == AIMP.Protocol.RemoteCharacterStateNames.Ready
            && value.RemoteCharacterBase != 0;
        _spawnCharacterBase.Text = ready
            ? $"0x{value.RemoteCharacterBase:X8}"
            : SpawnTrackerStatusText(value.RemoteCharacterState);
        _copySpawnCharacterBase.Enabled = ready;
    }

    private static string SpawnTrackerStatusText(string state) => state switch
    {
        AIMP.Protocol.RemoteCharacterStateNames.Pending => "Waiting for CharacterCreate...",
        AIMP.Protocol.RemoteCharacterStateNames.Failed => "CharacterBase tracking failed",
        _ => "Not tracked"
    };

    private void OnCopySpawnCharacterBaseClick(object? sender, EventArgs eventArgs)
    {
        if (!_copySpawnCharacterBase.Enabled) return;
        Clipboard.SetText(_spawnCharacterBase.Text);
        _result.Text = $"CharacterBase {_spawnCharacterBase.Text} copied to clipboard.";
        _result.ForeColor = SystemColors.ControlText;
    }

    private void AddParameter(GameCommandParameterDefinition parameter)
    {
        var row = _parameters.RowCount++;
        var label = new Label { Text = parameter.Label, AutoSize = true,
            Anchor = AnchorStyles.Left, Margin = new Padding(3, 8, 8, 3) };
        var editor = CreateEditor(parameter);
        editor.Margin = new Padding(3, 3, 3, 8);
        _toolTip.SetToolTip(label, parameter.Description);
        _toolTip.SetToolTip(editor, parameter.Description);
        _parameters.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        _parameters.Controls.Add(label, 0, row);
        _parameters.Controls.Add(editor, 1, row);
        _editors.Add(parameter.Key, editor);
    }

    private static Control CreateEditor(GameCommandParameterDefinition parameter) =>
        parameter.Kind switch
        {
            GameCommandParameterKind.Text => CreateTextEditor(parameter, false),
            GameCommandParameterKind.MultilineText => CreateTextEditor(parameter, true),
            GameCommandParameterKind.Decimal => CreateNumberEditor(parameter),
            GameCommandParameterKind.Choice => CreateChoiceEditor(parameter),
            GameCommandParameterKind.Boolean => CreateBooleanEditor(parameter),
            _ => throw new InvalidOperationException("Unknown command parameter type.")
        };

    private static TextBox CreateTextEditor(
        GameCommandParameterDefinition parameter,
        bool multiline) => new()
    {
        Dock = DockStyle.Top,
        Text = parameter.DefaultValue,
        Multiline = multiline,
        ScrollBars = multiline ? ScrollBars.Vertical : ScrollBars.None,
        Height = multiline ? 90 : 24
    };

    private static NumericUpDown CreateNumberEditor(GameCommandParameterDefinition parameter)
    {
        var number = new NumericUpDown
        {
            Dock = DockStyle.Top, Minimum = parameter.Minimum, Maximum = parameter.Maximum,
            Increment = parameter.Increment, DecimalPlaces = parameter.DecimalPlaces
        };
        if (decimal.TryParse(parameter.DefaultValue, NumberStyles.Float,
                CultureInfo.InvariantCulture, out var value))
            number.Value = Math.Clamp(value, number.Minimum, number.Maximum);
        return number;
    }

    private static ComboBox CreateChoiceEditor(GameCommandParameterDefinition parameter)
    {
        var choice = new ComboBox
        {
            Dock = DockStyle.Top, DropDownStyle = ComboBoxStyle.DropDownList,
            DisplayMember = nameof(GameCommandChoice.Label), ValueMember = nameof(GameCommandChoice.Value)
        };
        foreach (var item in parameter.Choices ?? Array.Empty<GameCommandChoice>())
            choice.Items.Add(item);
        choice.SelectedIndex = FindDefaultChoice(parameter);
        return choice;
    }

    private static CheckBox CreateBooleanEditor(GameCommandParameterDefinition parameter)
    {
        var checkBox = new CheckBox
        {
            AutoSize = true,
            Checked = string.Equals(parameter.DefaultValue, "1", StringComparison.Ordinal),
            Anchor = AnchorStyles.Left
        };
        void UpdateText() => checkBox.Text = checkBox.Checked
            ? "Security enabled (STRICT)"
            : "Security disabled (OPEN)";
        checkBox.CheckedChanged += (_, _) => UpdateText();
        UpdateText();
        return checkBox;
    }

    private static int FindDefaultChoice(GameCommandParameterDefinition parameter)
    {
        var choices = parameter.Choices ?? Array.Empty<GameCommandChoice>();
        var index = choices.ToList().FindIndex(value =>
            string.Equals(value.Value, parameter.DefaultValue, StringComparison.Ordinal));
        return index >= 0 ? index : choices.Count > 0 ? 0 : -1;
    }

    private async void OnExecuteClick(object? sender, EventArgs eventArgs)
    {
        if (Interlocked.Exchange(ref _executing, 1) != 0) return;
        try
        {
            await ExecuteSelectedCommandAsync();
        }
        finally
        {
            Interlocked.Exchange(ref _executing, 0);
            UpdateState(_controller.Snapshot);
        }
    }

    private async Task ExecuteSelectedCommandAsync()
    {
        try
        {
            var command = SelectedCommand();
            _execute.Enabled = false;
            _result.Text = command.Target switch
            {
                GameCommandExecutionTarget.Multiplayer => "Broadcasting command",
                GameCommandExecutionTarget.Launcher => "Running launcher command",
                _ => _controller.Snapshot.EngineThreadReady ? "Command submitted" : "Command queued"
            };
            var values = ReadValues(command);
            var response = await DispatchCommandAsync(command, values);

            ShowCommandResult(response);
        }
        catch (Exception exception)
        {
            ShowCommandError(exception.Message);
        }
    }

    private Task<AIMP.Protocol.GameCommandResultPayload> DispatchCommandAsync(
        GameCommandDefinition command,
        IReadOnlyDictionary<string, string> values) =>
        command.Target != GameCommandExecutionTarget.Native
            ? _multiplayer.ExecuteLauncherCommandAsync(command.Name, values)
            : _controller.ExecuteCommandAsync(command.Name, values);

    private GameCommandDefinition SelectedCommand() =>
        _commandSelector.SelectedItem as GameCommandDefinition
        ?? throw new InvalidOperationException("Select a command.");

    private IReadOnlyDictionary<string, string> ReadValues(GameCommandDefinition command)
    {
        var values = command.Parameters.ToDictionary(
            parameter => parameter.Key,
            parameter => ReadEditorValue(_editors[parameter.Key], parameter),
            StringComparer.Ordinal);
        return GameCommandCatalog.ValidateAndNormalize(command, values);
    }


    private static bool IsCharacterActionTest(GameCommandDefinition command) =>
        string.Equals(command.Name, AIMP.Protocol.GameCommandNames.CharacterActionTest,
            StringComparison.Ordinal);

    private static bool IsDirectMove(GameCommandDefinition command) =>
        string.Equals(command.Name, AIMP.Protocol.GameCommandNames.CharacterDirectMove,
            StringComparison.Ordinal);

    private static bool IsSpawnCharacter(GameCommandDefinition command) =>
        string.Equals(command.Name, AIMP.Protocol.GameCommandNames.SpawnCharacter,
            StringComparison.Ordinal);

    private static string ReadEditorValue(
        Control editor,
        GameCommandParameterDefinition parameter) => editor switch
    {
        TextBox text => text.Text,
        NumericUpDown number => number.Value.ToString(
            $"F{parameter.DecimalPlaces}", CultureInfo.InvariantCulture),
        ComboBox { SelectedItem: GameCommandChoice choice } => choice.Value,
        CheckBox checkBox => checkBox.Checked ? "1" : "0",
        _ => throw new InvalidOperationException($"Invalid editor for {parameter.Label}.")
    };

    private void ShowCommandResult(AIMP.Protocol.GameCommandResultPayload result)
    {
        _result.Text = NativeRuntimeController.CommandResultText(result);
        _result.ForeColor = result.Executed || NativeRuntimeController.IsQueuedResult(result)
            ? SystemColors.ControlText : Color.DarkRed;
    }

    private void ShowCommandError(string message)
    {
        _result.Text = message;
        _result.ForeColor = Color.DarkRed;
    }

    private void OnStateChanged(object? sender, NativeRuntimeSnapshot snapshot)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) BeginInvoke(new Action(() => UpdateState(snapshot)));
        else UpdateState(snapshot);
    }

    private void OnMultiplayerStateChanged(
        object? sender,
        MultiplayerSessionView value)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) BeginInvoke(new Action(() => UpdateState(_controller.Snapshot)));
        else UpdateState(_controller.Snapshot);
    }

    private void UpdateState(NativeRuntimeSnapshot value)
    {
        _state.Text = SelectedRuntimeStateText(value);
        if (SelectedCommandIsSpawnCharacter()) UpdateSpawnCharacterAddress(value);
        if (SelectedCommandIsNative() && !string.IsNullOrWhiteSpace(value.LastCommandResult))
            _result.Text = value.LastCommandResult;
        var idle = Volatile.Read(ref _executing) == 0;
        _execute.Enabled = idle;
        var actionReady = idle && value.AiRunning && value.DllInjected && value.IpcConnected
            && value.EngineHooksInstalled;
        foreach (var button in _characterActionButtons) button.Enabled = actionReady;
    }

    private bool SelectedCommandIsSpawnCharacter() =>
        _commandSelector.SelectedItem is GameCommandDefinition command && IsSpawnCharacter(command);

    private bool SelectedCommandIsNative() =>
        _commandSelector.SelectedItem is not GameCommandDefinition command
        || command.Target == GameCommandExecutionTarget.Native;

    private string SelectedRuntimeStateText(NativeRuntimeSnapshot value)
    {
        if (_commandSelector.SelectedItem is not GameCommandDefinition command)
            return RuntimeStateText(value);
        if (command.Target == GameCommandExecutionTarget.Native) return RuntimeStateText(value);
        var multiplayer = _multiplayer.State;
        if (!multiplayer.Connected) return "Create or join a lobby before executing this command.";
        if (command.Target == GameCommandExecutionTarget.Launcher)
            return "Local launcher command ready. It runs only on this peer.";
        return multiplayer.IsHost
            ? "Host ready to broadcast the network command."
            : "Only the lobby host can execute this command.";
    }

    private static string RuntimeStateText(NativeRuntimeSnapshot value)
    {
        if (!value.AiRunning) return "AI.exe is not running";
        if (!value.DllInjected) return "DLL not injected";
        if (!value.IpcConnected) return "DLL IPC is not connected";
        if (!value.EngineHooksInstalled) return "Engine hooks are unavailable; commands cannot reach the engine thread.";
        if (!value.EngineThreadReady) return "Engine hooks are installed; resolved commands will queue until the first callback.";
        return $"Ready on engine thread {value.EngineThreadId}.";
    }

    private void OnFormClosed(object? sender, FormClosedEventArgs eventArgs)
    {
        _controller.StateChanged -= OnStateChanged;
        _multiplayer.StateChanged -= OnMultiplayerStateChanged;
        _toolTip.Dispose();
    }
}
