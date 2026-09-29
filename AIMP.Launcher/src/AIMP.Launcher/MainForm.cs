using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed class MainForm : Form
{
    private readonly NativeRuntimeController _controller;
    private readonly LauncherSettingsStore _settingsStore = new();
    private readonly ScaleformBridgeListener _bridge;
    private readonly GameLaunchCoordinator _gameLauncher;
    private readonly MultiplayerSessionCoordinator _multiplayer;
    private readonly PerformanceIsolationState _performanceIsolation = new();
    private readonly Dictionary<PerformanceIsolationModule, ToolStripMenuItem> _performanceItems = new();
    private readonly Button _launchButton = new() { Text = "Launch Game", AutoSize = true };
    private readonly Button _settingsButton = new() { Text = "Settings", AutoSize = true };
    private readonly Button _injectButton = new() { Text = "Inject DLL", AutoSize = true };
    private readonly Button _exportButton = new() { Text = "Export Diagnostics", AutoSize = true };
    private readonly Button _commandButton = new() { Text = "Execute Command", AutoSize = true };
    private readonly Button _characterBaseButton = new()
        { Text = "CharacterBase Calculator", AutoSize = true };
    private readonly Button _worldManifestButton = new()
        { Text = "World Manifest", AutoSize = true };
    private readonly Label _processStatus = CreateStatusValue();
    private readonly Label _injectionStatus = CreateStatusValue();
    private readonly Label _ipcStatus = CreateStatusValue();
    private readonly Label _threadStatus = CreateStatusValue();
    private readonly Label _bridgeStatus = CreateStatusValue();
    private readonly Label _networkStatus = CreateStatusValue();
    private readonly Label _playerPositionStatus = CreateStatusValue();
    private readonly Label _remoteCharacterStatus = CreateStatusValue();
    private readonly RichTextBox _log = new();
    private readonly System.Windows.Forms.Timer _refreshTimer = new() { Interval = 1000 };
    private readonly System.Windows.Forms.Timer _positionTimer = new() { Interval = 16 };
    private readonly LocalPlayerPositionReader _positionReader = new();
    private readonly System.Collections.Concurrent.ConcurrentQueue<string> _pendingUiLogs = new();
    private int _busy;
    private int _bridgeAutoInjectionRunning;

    internal MainForm(NativeRuntimeController controller)
    {
        _controller = controller ?? throw new ArgumentNullException(nameof(controller));
        _bridge = new ScaleformBridgeListener(_controller.Diagnostics);
        _gameLauncher = new GameLaunchCoordinator(_settingsStore,
            new UiPakInstaller(_controller.Diagnostics), _bridge, _controller.Diagnostics);
        _multiplayer = new MultiplayerSessionCoordinator(
            _controller, _settingsStore, _bridge, _performanceIsolation);
        ConfigureWindow();
        Controls.Add(BuildInterface());
        SubscribeEvents();
        SeedLog();
        UpdateState(_controller.Snapshot);
        UpdateBridgeState(_bridge.State);
        UpdateMultiplayerState(_multiplayer.State);
    }

    private void ConfigureWindow()
    {
        Text = $"AIMP Launcher v{ProductInfo.LauncherVersion}";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(760, 500);
        ClientSize = new Size(900, 610);
        Icon = SystemIcons.Application;
    }

    private Control BuildInterface()
    {
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(12), ColumnCount = 1, RowCount = 4
        };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.Controls.Add(BuildPerformanceMenu(), 0, 0);
        root.Controls.Add(BuildActionBar(), 0, 1);
        root.Controls.Add(BuildStatusPanel(), 0, 2);
        root.Controls.Add(BuildLogPanel(), 0, 3);
        return root;
    }

    private Control BuildPerformanceMenu()
    {
        var strip = new MenuStrip
        {
            Dock = DockStyle.Fill,
            GripStyle = ToolStripGripStyle.Hidden,
            Margin = new Padding(0, 0, 0, 8)
        };
        var root = new ToolStripMenuItem("Performance Isolation");
        AddPerformanceItem(root, PerformanceIsolationModule.LocalPlayerSampling,
            "Local player memory sampling",
            "Stops launcher ReadProcessMemory sampling for local position/yaw/movement profile.");
        AddPerformanceItem(root, PerformanceIsolationModule.PositionNetworkTransmit,
            "Position network transmit",
            "Stops publishing local position snapshots while keeping the lobby connected.");
        AddPerformanceItem(root, PerformanceIsolationModule.RemoteCharacterSpawn,
            "Remote Character spawn / convergence",
            "Stops new remote Character spawn/convergence. Existing Characters are not despawned; disable before join/rejoin for a clean test.");
        AddPerformanceItem(root, PerformanceIsolationModule.RemoteMovement,
            "Remote movement replication",
            "Stops remote Character movement commands and disables the native movement follower.");
        AddPerformanceItem(root, PerformanceIsolationModule.EquipmentSync,
            "Equipment replication",
            "Stops PlayerSync equipment snapshots and remote equipment convergence.");
        AddPerformanceItem(root, PerformanceIsolationModule.ActionSync,
            "Aim / Primary replication",
            "Stops PlayerSync Aim/Primary events and releases currently-applied remote actions.");
        root.DropDownItems.Add(new ToolStripSeparator());
        AddPerformanceItem(root, PerformanceIsolationModule.WorldSync,
            "WorldSync / CATHODE",
            "Closes the WorldSync bus and bypasses native WorldSync/CATHODE replication hooks.");
        AddPerformanceItem(root, PerformanceIsolationModule.UiBridge,
            "Scaleform / UI bridge",
            "Stops the launcher Scaleform TCP bridge while leaving lobby IPC/network connected.");
        AddPerformanceItem(root, PerformanceIsolationModule.EngineThreadEvidence,
            "Native engine-thread evidence",
            "Disables native engine-thread evidence/capture work.");
        AddPerformanceItem(root, PerformanceIsolationModule.CharacterLifecycleCapture,
            "Native Character lifecycle capture",
            "Disables native Character create/lifecycle observation while preserving safety cleanup.");
        AddPerformanceItem(root, PerformanceIsolationModule.GameThreadPumps,
            "Native game-thread pumps",
            "Disables AIMP game-thread dispatcher/replay/movement pumps. The IPC control channel stays live so this can be re-enabled.");
        AddPerformanceItem(root, PerformanceIsolationModule.Diagnostics,
            "Diagnostics / log recording",
            "Stops launcher/native diagnostic log recording and the locomotion flight recorder.");
        root.DropDownItems.Add(new ToolStripSeparator());
        var enableAll = new ToolStripMenuItem("Enable all");
        enableAll.Click += async (_, _) => await ApplyAllPerformanceModulesAsync(true);
        root.DropDownItems.Add(enableAll);
        var disableAll = new ToolStripMenuItem("Disable all (keep lobby connected)");
        disableAll.Click += async (_, _) => await ApplyAllPerformanceModulesAsync(false);
        root.DropDownItems.Add(disableAll);
        strip.Items.Add(root);
        return strip;
    }

    private void AddPerformanceItem(
        ToolStripMenuItem root,
        PerformanceIsolationModule module,
        string text,
        string toolTip)
    {
        var item = new ToolStripMenuItem(text)
        {
            Checked = true,
            CheckOnClick = false,
            Tag = module,
            ToolTipText = toolTip
        };
        item.Click += OnPerformanceModuleClick;
        _performanceItems[module] = item;
        root.DropDownItems.Add(item);
    }

    private async void OnPerformanceModuleClick(object? sender, EventArgs eventArgs)
    {
        if (sender is not ToolStripMenuItem item || item.Tag is not PerformanceIsolationModule module)
            return;
        await ApplyPerformanceModuleAsync(module, !item.Checked);
    }

    private async Task ApplyPerformanceModuleAsync(
        PerformanceIsolationModule module, bool enabled)
    {
        if (!_performanceItems.TryGetValue(module, out var item) || !item.Enabled) return;
        item.Enabled = false;
        try
        {
            switch (module)
            {
                case PerformanceIsolationModule.UiBridge:
                    if (enabled) StartBridge();
                    else await _bridge.StopAsync().ConfigureAwait(true);
                    _performanceIsolation.SetEnabled(module, enabled);
                    break;

                case PerformanceIsolationModule.Diagnostics:
                    if (enabled) _controller.Diagnostics.SetEnabled(true);
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.NativeDiagnostics, enabled).ConfigureAwait(true);
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.LocomotionFlight, enabled).ConfigureAwait(true);
                    _performanceIsolation.SetEnabled(module, enabled);
                    if (!enabled) _controller.Diagnostics.SetEnabled(false);
                    break;

                default:
                    await _multiplayer.ApplyPerformanceIsolationModuleAsync(module, enabled)
                        .ConfigureAwait(true);
                    break;
            }
            item.Checked = enabled;
            if (_controller.Diagnostics.Enabled)
                _controller.Diagnostics.Log("Performance",
                    $"Isolation module {module}={(enabled ? "ON" : "OFF")}.");
        }
        catch (Exception exception)
        {
            if (module == PerformanceIsolationModule.Diagnostics)
                _controller.Diagnostics.SetEnabled(true);
            ShowError($"Performance isolation update failed for {item.Text}: {exception.Message}");
        }
        finally
        {
            item.Enabled = true;
        }
    }

    private async Task ApplyAllPerformanceModulesAsync(bool enabled)
    {
        var disableOrder = new[]
        {
            PerformanceIsolationModule.LocalPlayerSampling,
            PerformanceIsolationModule.PositionNetworkTransmit,
            PerformanceIsolationModule.RemoteCharacterSpawn,
            PerformanceIsolationModule.EquipmentSync,
            PerformanceIsolationModule.ActionSync,
            PerformanceIsolationModule.RemoteMovement,
            PerformanceIsolationModule.WorldSync,
            PerformanceIsolationModule.UiBridge,
            PerformanceIsolationModule.EngineThreadEvidence,
            PerformanceIsolationModule.CharacterLifecycleCapture,
            PerformanceIsolationModule.Diagnostics,
            PerformanceIsolationModule.GameThreadPumps
        };
        IEnumerable<PerformanceIsolationModule> order =
            enabled ? disableOrder.Reverse() : disableOrder;
        foreach (var module in order)
        {
            if (_performanceItems.TryGetValue(module, out var item) && item.Checked != enabled)
                await ApplyPerformanceModuleAsync(module, enabled).ConfigureAwait(true);
        }
    }

    private Control BuildActionBar()
    {
        var panel = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = true,
            WrapContents = true,
            Margin = new Padding(0, 0, 0, 10)
        };
        panel.Controls.Add(_launchButton);
        panel.Controls.Add(_settingsButton);
        panel.Controls.Add(_injectButton);
        panel.Controls.Add(_exportButton);
        panel.Controls.Add(_commandButton);
        panel.Controls.Add(_characterBaseButton);
        panel.Controls.Add(_worldManifestButton);
        return panel;
    }

    private Control BuildStatusPanel()
    {
        var table = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 8
        };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 155));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        AddStatusRow(table, 0, "AI.exe", _processStatus);
        AddStatusRow(table, 1, "Injection", _injectionStatus);
        AddStatusRow(table, 2, "IPC connection", _ipcStatus);
        AddStatusRow(table, 3, "Engine thread", _threadStatus);
        AddStatusRow(table, 4, "UI bridge", _bridgeStatus);
        AddStatusRow(table, 5, "Lobby network", _networkStatus);
        AddStatusRow(table, 6, "Local player position", _playerPositionStatus);
        AddStatusRow(table, 7, "Remote Character", _remoteCharacterStatus);
        return CreateStatusGroup(table);
    }

    private static Control CreateStatusGroup(Control table) => new GroupBox
    {
        Text = "Runtime status", Dock = DockStyle.Fill,
        AutoSize = true, Padding = new Padding(10), Controls = { table }
    };

    private Control BuildLogPanel()
    {
        _log.Dock = DockStyle.Fill;
        _log.ReadOnly = true;
        _log.BackColor = Color.FromArgb(20, 20, 20);
        _log.ForeColor = Color.Gainsboro;
        _log.Font = new Font(FontFamily.GenericMonospace, 9f);
        _log.DetectUrls = false;
        return new GroupBox { Text = "Log", Dock = DockStyle.Fill,
            Padding = new Padding(10), Margin = new Padding(0, 10, 0, 0), Controls = { _log } };
    }

    private static Label CreateStatusValue() => new()
    {
        AutoSize = true,
        Anchor = AnchorStyles.Left,
        ForeColor = Color.DimGray
    };

    private static void AddStatusRow(
        TableLayoutPanel table,
        int row,
        string name,
        Control value)
    {
        var label = new Label { Text = name, AutoSize = true, Anchor = AnchorStyles.Left };
        label.Font = new Font(label.Font, FontStyle.Bold);
        table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        table.Controls.Add(label, 0, row);
        table.Controls.Add(value, 1, row);
    }

    private void SubscribeEvents()
    {
        _launchButton.Click += OnLaunchClick;
        _settingsButton.Click += OnSettingsClick;
        _injectButton.Click += OnInjectClick;
        _exportButton.Click += OnExportClick;
        _commandButton.Click += OnCommandClick;
        _characterBaseButton.Click += OnCharacterBaseClick;
        _worldManifestButton.Click += OnWorldManifestClick;
        _controller.StateChanged += OnStateChanged;
        _controller.Diagnostics.EntryWritten += OnLogEntry;
        _bridge.StateChanged += OnBridgeStateChanged;
        _multiplayer.StateChanged += OnMultiplayerStateChanged;
        SubscribeTimersAndWindow();
    }

    private void SubscribeTimersAndWindow()
    {
        _refreshTimer.Tick += OnRefreshTick;
        _positionTimer.Tick += OnPositionTick;
        Shown += OnShown;
        FormClosed += OnFormClosed;
    }

    private void SeedLog()
    {
        foreach (var entry in _controller.Diagnostics.SnapshotEntries()) AppendLog(entry);
    }

    private void OnShown(object? sender, EventArgs eventArgs)
    {
        StartBridge();
        _controller.RefreshProcessStatus();
        _refreshTimer.Start();
        _positionTimer.Start();
    }

    private void StartBridge()
    {
        try { _bridge.Start(); }
        catch (Exception exception)
        {
            _controller.Diagnostics.Log("UI Bridge", $"Bridge startup failed: {exception.Message}");
            UpdateBridgeFailure();
        }
    }

    private async void OnLaunchClick(object? sender, EventArgs eventArgs)
    {
        if (!TryBeginOperation()) return;
        try
        {
            if (!EnsureGameDirectory()) return;
            await _gameLauncher.LaunchAsync();
            _controller.RefreshProcessStatus();
        }
        catch (Exception exception) { ShowError($"Game launch failed: {exception.Message}"); }
        finally { EndOperation(); }
    }

    private bool EnsureGameDirectory()
    {
        try
        {
            var current = _settingsStore.Load().GameDirectory;
            if (GameInstallation.TryResolve(current, out _, out _)) return true;
        }
        catch (Exception exception)
        { _controller.Diagnostics.Log("Settings", $"Settings load failed: {exception.Message}"); }
        return ShowSettingsDialog();
    }

    private void OnSettingsClick(object? sender, EventArgs eventArgs) => ShowSettingsDialog();

    private bool ShowSettingsDialog()
    {
        using var dialog = new SettingsForm(_settingsStore, _controller.Diagnostics);
        return dialog.ShowDialog(this) == DialogResult.OK;
    }

    private async void OnInjectClick(object? sender, EventArgs eventArgs)
    {
        if (!TryBeginOperation()) return;
        try { await _controller.InjectAsync(); }
        catch (Exception exception) { ShowError(exception.Message); }
        finally { EndOperation(); }
    }

    private async void OnExportClick(object? sender, EventArgs eventArgs)
    {
        if (!TryBeginOperation()) return;
        try
        {
            var path = SelectDiagnosticsPath();
            if (path is not null) await ExportDiagnosticsAsync(path);
        }
        finally { EndOperation(); }
    }

    private string? SelectDiagnosticsPath()
    {
        using var dialog = new SaveFileDialog
        {
            Filter = "ZIP archive (*.zip)|*.zip",
            DefaultExt = "zip",
            AddExtension = true,
            FileName = $"AIMP-Diagnostics-{DateTime.Now:yyyyMMdd-HHmmss}.zip"
        };
        return dialog.ShowDialog(this) == DialogResult.OK ? dialog.FileName : null;
    }

    private async Task ExportDiagnosticsAsync(string path)
    {
        try
        {
            var result = await _controller.ExportDiagnosticsAsync(path);
            MessageBox.Show(this, $"Diagnostics exported to:{Environment.NewLine}{result}",
                "AIMP Launcher", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception exception)
        { ShowError($"Diagnostics export failed: {exception.Message}"); }
    }

    private void OnCommandClick(object? sender, EventArgs eventArgs)
    {
        using var dialog = new ExecuteCommandForm(_controller, _multiplayer);
        dialog.ShowDialog(this);
    }

    private void OnCharacterBaseClick(object? sender, EventArgs eventArgs)
    {
        using var dialog = new CharacterBaseCalculatorForm();
        dialog.ShowDialog(this);
    }

    private void OnWorldManifestClick(object? sender, EventArgs eventArgs)
    {
        if (!EnsureGameDirectory()) return;
        using var dialog = new WorldManifestCompilerForm(_controller.Diagnostics, _settingsStore);
        dialog.ShowDialog(this);
    }

    private bool TryBeginOperation()
    {
        if (Interlocked.Exchange(ref _busy, 1) != 0) return false;
        UpdateActionAvailability();
        return true;
    }

    private void EndOperation()
    {
        Interlocked.Exchange(ref _busy, 0);
        UpdateActionAvailability();
    }

    private void UpdateActionAvailability()
    {
        var idle = Volatile.Read(ref _busy) == 0;
        _launchButton.Enabled = idle && !_controller.Snapshot.AiRunning;
        _settingsButton.Enabled = idle;
        _injectButton.Enabled = idle;
        _exportButton.Enabled = idle;
        _commandButton.Enabled = idle;
        _characterBaseButton.Enabled = idle;
        _worldManifestButton.Enabled = idle;
    }

    private void OnRefreshTick(object? sender, EventArgs eventArgs)
    {
        FlushPendingLogs();
        _controller.RefreshProcessStatus();
    }

    private void OnPositionTick(object? sender, EventArgs eventArgs)
    {
        var runtime = _controller.Snapshot;
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.LocalPlayerSampling))
        {
            _playerPositionStatus.Text = "Disabled (Performance Isolation)";
            _playerPositionStatus.ForeColor = Color.DarkOrange;
            _multiplayer.UpdateLocalRuntime(
                runtime, false, default, false, default, false, default);
            return;
        }
        if (_positionReader.TryRead(runtime, out var position))
        {
            var yawAvailable = _positionReader.TryReadYaw(runtime, out var yaw);
            var movementProfileAvailable =
                _positionReader.TryReadMovementProfile(runtime, out var movementProfile);
            ShowPlayerPosition(position, yawAvailable ? yaw : (float?)null);
            _multiplayer.UpdateLocalRuntime(
                runtime, true, position, yawAvailable, yaw,
                movementProfileAvailable, movementProfile);
            return;
        }
        ShowPositionUnavailable(runtime);
        _multiplayer.UpdateLocalRuntime(
            runtime, false, default, false, default, false, default);
    }

    private void ShowPlayerPosition(LocalPlayerPosition position, float? yaw)
    {
        _playerPositionStatus.Text = position.ToDisplayText(yaw);
        _playerPositionStatus.ForeColor = Color.DarkGreen;
    }

    private void ShowPositionUnavailable(NativeRuntimeSnapshot runtime)
    {
        _playerPositionStatus.Text = runtime.AiRunning
            ? "Position unavailable" : "Waiting for AI.exe";
        _playerPositionStatus.ForeColor = runtime.AiRunning
            ? Color.DarkOrange : Color.DimGray;
    }

    private void OnStateChanged(object? sender, NativeRuntimeSnapshot snapshot)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) BeginInvoke(new Action(() => UpdateState(snapshot)));
        else UpdateState(snapshot);
    }

    private void UpdateState(NativeRuntimeSnapshot value)
    {
        _processStatus.Text = value.AiRunning ? $"Running (PID={value.ProcessId})" : "Not running";
        _injectionStatus.Text = value.DllInjected ? "Injected" : "Not injected";
        _ipcStatus.Text = value.IpcConnected ? "Connected" : "Disconnected";
        _threadStatus.Text = EngineThreadText(value);
        UpdateRemoteCharacterState(value);
        if (!value.AiRunning) ShowPositionUnavailable(value);
        _processStatus.ForeColor = value.AiRunning ? Color.DarkGreen : Color.DimGray;
        _injectionStatus.ForeColor = value.DllInjected ? Color.DarkGreen : Color.DimGray;
        _ipcStatus.ForeColor = value.IpcConnected ? Color.DarkGreen : Color.DimGray;
        _threadStatus.ForeColor = EngineThreadColor(value);
        UpdateActionAvailability();
    }

    private void UpdateRemoteCharacterState(NativeRuntimeSnapshot value)
    {
        _remoteCharacterStatus.Text = RemoteCharacterStatusText(value);
        _remoteCharacterStatus.ForeColor = RemoteCharacterStatusColor(value);
    }

    private static string RemoteCharacterStatusText(NativeRuntimeSnapshot value)
    {
        if (!value.DllInjected || !value.IpcConnected) return "Unavailable";
        if (!value.CharacterCreateHookInstalled) return "Tracker hook unavailable";
        return value.RemoteCharacterState switch
        {
            RemoteCharacterStateNames.Pending => $"Spawn pending (key={value.RemoteCharacterSpawnKey})",
            RemoteCharacterStateNames.Ready => $"Ready (CharacterBase=0x{value.RemoteCharacterBase:X8})",
            RemoteCharacterStateNames.Failed => $"Tracking failed (key={value.RemoteCharacterSpawnKey})",
            _ => "Not spawned"
        };
    }

    private static Color RemoteCharacterStatusColor(NativeRuntimeSnapshot value) =>
        value.RemoteCharacterState switch
        {
            RemoteCharacterStateNames.Ready => Color.DarkGreen,
            RemoteCharacterStateNames.Pending => Color.DarkOrange,
            RemoteCharacterStateNames.Failed => Color.DarkRed,
            _ => Color.DimGray
        };

    private void OnBridgeStateChanged(object? sender, ScaleformBridgeState state)
    {
        if (IsDisposed || Disposing) return;
        if (state.Connected) _ = EnsureBridgeAutoInjectionAsync();
        if (InvokeRequired) BeginInvoke(new Action(() => UpdateBridgeState(state)));
        else UpdateBridgeState(state);
    }

    private async Task EnsureBridgeAutoInjectionAsync()
    {
        var current = _controller.Snapshot;
        if ((current.DllInjected && current.IpcConnected) ||
            Interlocked.CompareExchange(ref _bridgeAutoInjectionRunning, 1, 0) != 0)
            return;

        try
        {
            _controller.Diagnostics.Log(
                "Injection",
                "UI Bridge connected for the first time; automatic DLL injection is starting.");
            for (var attempt = 1; attempt <= 10; attempt++)
            {
                if (IsDisposed || Disposing || !_bridge.State.Connected) return;
                try
                {
                    _controller.RefreshProcessStatus();
                    await _controller.InjectAsync().ConfigureAwait(false);
                    current = _controller.Snapshot;
                    if (current.DllInjected && current.IpcConnected)
                    {
                        _controller.Diagnostics.Log(
                            "Injection",
                            "Automatic DLL injection/IPC handshake completed after UI Bridge connection.");
                        return;
                    }
                }
                catch (Exception exception)
                {
                    _controller.Diagnostics.Log(
                        "Injection",
                        $"Automatic injection attempt {attempt}/10 failed: {exception.Message}");
                }
                await Task.Delay(500).ConfigureAwait(false);
            }
            _controller.Diagnostics.Log(
                "Injection",
                "Automatic injection did not complete while the UI Bridge remained connected; the manual Inject DLL button remains available.");
        }
        finally
        {
            Volatile.Write(ref _bridgeAutoInjectionRunning, 0);
        }
    }

    private void UpdateBridgeState(ScaleformBridgeState state)
    {
        _bridgeStatus.Text = BridgeStateText(state);
        _bridgeStatus.ForeColor = state.Connected ? Color.DarkGreen
            : state.Running ? Color.DarkOrange : Color.DimGray;
    }

    private void UpdateBridgeFailure()
    {
        _bridgeStatus.Text = "Failed to start";
        _bridgeStatus.ForeColor = Color.DarkRed;
    }

    private void OnMultiplayerStateChanged(object? sender, MultiplayerSessionView state)
    {
        if (IsDisposed || Disposing) return;
        if (InvokeRequired) BeginInvoke(new Action(() => UpdateMultiplayerState(state)));
        else UpdateMultiplayerState(state);
    }

    private void UpdateMultiplayerState(MultiplayerSessionView state)
    {
        _networkStatus.Text = MultiplayerStateText(state);
        _networkStatus.ForeColor = state.Connected ? Color.DarkGreen : Color.DimGray;
    }

    private static string MultiplayerStateText(MultiplayerSessionView state)
    {
        if (!state.Connected) return "Disconnected";
        if (state.RemoteGameplayReady) return $"{state.NetworkState} - remote gameplay ready";
        if (state.LocalGameplayReady) return $"{state.NetworkState} - local gameplay ready";
        return state.RemotePlayerName.Length == 0
            ? state.NetworkState : $"{state.NetworkState} with {state.RemotePlayerName}";
    }

    private static string BridgeStateText(ScaleformBridgeState state)
    {
        if (state.Connected) return $"Connected ({state.ConnectedClients} client(s))";
        return state.Running ? "Listening (waiting for UI)" : "Stopped";
    }

    private static string EngineThreadText(NativeRuntimeSnapshot value)
    {
        if (!value.DllInjected) return "Not available (DLL not injected)";
        if (!value.IpcConnected) return "Not available (IPC disconnected)";
        if (value.EngineThreadReady)
            return $"Ready (ID={value.EngineThreadId}, {value.EngineThreadSource})";
        return value.EngineHooksInstalled
            ? "Searching continuously for an engine callback"
            : "Unavailable (engine hooks not installed)";
    }

    private static Color EngineThreadColor(NativeRuntimeSnapshot value) =>
        !value.DllInjected || !value.IpcConnected ? Color.DimGray
            : value.EngineThreadReady ? Color.DarkGreen
                : value.EngineHooksInstalled ? Color.DarkOrange : Color.DarkRed;

    private void OnLogEntry(object? sender, string entry)
    {
        if (IsDisposed || Disposing) return;
        _pendingUiLogs.Enqueue(entry);
    }

    private void FlushPendingLogs()
    {
        if (_pendingUiLogs.IsEmpty || IsDisposed || Disposing) return;
        var builder = new System.Text.StringBuilder();
        var visible = 0;
        while (visible < 1_000 && _pendingUiLogs.TryDequeue(out var entry))
        {
            builder.AppendLine(entry);
            visible++;
        }

        var omitted = 0;
        while (_pendingUiLogs.TryDequeue(out _)) omitted++;
        if (omitted != 0)
        {
            builder.Append("[Launcher] ").Append(omitted)
                .AppendLine(" additional UI log lines omitted; the full diagnostics remain on disk.");
        }
        if (builder.Length == 0) return;
        _log.AppendText(builder.ToString());
        TrimVisibleLog();
    }

    private void AppendLog(string entry)
    {
        _log.AppendText(entry + Environment.NewLine);
        TrimVisibleLog();
    }

    private void TrimVisibleLog()
    {
        const int maximumCharacters = 512_000;
        const int retainedCharacters = 384_000;
        if (_log.TextLength <= maximumCharacters)
        {
            _log.SelectionStart = _log.TextLength;
            _log.ScrollToCaret();
            return;
        }

        var remove = _log.TextLength - retainedCharacters;
        _log.Select(0, remove);
        _log.SelectedText = string.Empty;
        _log.SelectionStart = _log.TextLength;
        _log.ScrollToCaret();
    }

    private void ShowError(string message) => MessageBox.Show(
        this, message, "AIMP Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);

    private void OnFormClosed(object? sender, FormClosedEventArgs eventArgs)
    {
        _refreshTimer.Stop();
        _positionTimer.Stop();
        _refreshTimer.Dispose();
        _positionTimer.Dispose();
        _positionReader.Dispose();
        _bridge.StateChanged -= OnBridgeStateChanged;
        _multiplayer.StateChanged -= OnMultiplayerStateChanged;
        _multiplayer.Dispose();
        _bridge.Dispose();
        _controller.StateChanged -= OnStateChanged;
        _controller.Diagnostics.EntryWritten -= OnLogEntry;
    }
}
