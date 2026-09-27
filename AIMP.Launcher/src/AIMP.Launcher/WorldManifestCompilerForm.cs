namespace AIMP.Launcher;

internal sealed class WorldManifestCompilerForm : Form
{
    private const int SelectedColumnIndex = 0;
    private const int LevelColumnIndex = 1;
    private const int StatusColumnIndex = 2;
    private const int ProgressColumnIndex = 3;
    private const int ResultColumnIndex = 4;

    private readonly LauncherDiagnostics _diagnostics;
    private readonly LauncherSettingsStore _settingsStore;
    private readonly Label _productionPath = new()
        { AutoSize = true, ForeColor = Color.DimGray };
    private readonly Label _selectionSummary = new()
        { AutoSize = true, ForeColor = Color.DimGray, Text = "No levels loaded." };
    private readonly Button _refreshButton = new() { Text = "Refresh Levels", AutoSize = true };
    private readonly Button _selectAllButton = new() { Text = "Select All", AutoSize = true };
    private readonly Button _clearSelectionButton = new() { Text = "Clear Selection", AutoSize = true };
    private readonly Button _compileButton = new() { Text = "Create Selected Manifests", AutoSize = true };
    private readonly Button _closeButton = new()
        { Text = "Close", AutoSize = true, DialogResult = DialogResult.Cancel };
    private readonly ProgressBar _overallProgress = new()
        { Dock = DockStyle.Fill, Minimum = 0, Maximum = 100, Value = 0 };
    private readonly Label _status = new()
        { AutoSize = true, ForeColor = Color.DimGray, Text = "Loading available levels..." };
    private readonly DataGridView _levels = CreateLevelGrid();

    private bool _compiling;
    private bool _loadingLevels;

    internal WorldManifestCompilerForm(
        LauncherDiagnostics diagnostics,
        LauncherSettingsStore settingsStore)
    {
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
        _settingsStore = settingsStore ?? throw new ArgumentNullException(nameof(settingsStore));
        ConfigureWindow();
        Controls.Add(BuildInterface());
        SubscribeEvents();
        UpdateActionAvailability();
    }

    private void ConfigureWindow()
    {
        Text = "World Manifest Compiler";
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.Sizable;
        MaximizeBox = true;
        MinimizeBox = false;
        ClientSize = new Size(980, 620);
        MinimumSize = new Size(820, 520);
        AcceptButton = _compileButton;
        CancelButton = _closeButton;
    }

    private Control BuildInterface()
    {
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(12),
            ColumnCount = 1,
            RowCount = 8
        };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        root.Controls.Add(new Label
        {
            Text = "Available levels",
            AutoSize = true,
            Font = new Font(Font, FontStyle.Bold),
            Margin = new Padding(0, 0, 0, 4)
        }, 0, 0);

        root.Controls.Add(BuildProductionBar(), 0, 1);
        root.Controls.Add(BuildSelectionBar(), 0, 2);
        root.Controls.Add(_levels, 0, 3);
        _levels.Margin = new Padding(0, 6, 0, 10);

        root.Controls.Add(new Label
        {
            Text = "Overall progress",
            AutoSize = true,
            Font = new Font(Font, FontStyle.Bold),
            Margin = new Padding(0, 0, 0, 4)
        }, 0, 4);
        root.Controls.Add(_overallProgress, 0, 5);
        _overallProgress.Margin = new Padding(0, 0, 0, 6);
        root.Controls.Add(_status, 0, 6);
        _status.Margin = new Padding(0, 0, 0, 4);
        root.Controls.Add(BuildButtons(), 0, 7);
        return root;
    }

    private Control BuildProductionBar()
    {
        var panel = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = true,
            ColumnCount = 2,
            Margin = new Padding(0, 0, 0, 4)
        };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

        var info = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = true,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = true,
            Margin = new Padding(0)
        };
        info.Controls.Add(new Label
        {
            Text = "Source: ",
            AutoSize = true,
            Font = new Font(Font, FontStyle.Bold),
            Margin = new Padding(0, 4, 0, 0)
        });
        _productionPath.Margin = new Padding(0, 4, 0, 0);
        info.Controls.Add(_productionPath);

        panel.Controls.Add(info, 0, 0);
        panel.Controls.Add(_refreshButton, 1, 0);
        return panel;
    }

    private Control BuildSelectionBar()
    {
        var panel = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = true,
            ColumnCount = 2,
            Margin = new Padding(0)
        };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _selectionSummary.Anchor = AnchorStyles.Left;
        panel.Controls.Add(_selectionSummary, 0, 0);

        var buttons = new FlowLayoutPanel
        {
            AutoSize = true,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = new Padding(0)
        };
        buttons.Controls.Add(_selectAllButton);
        buttons.Controls.Add(_clearSelectionButton);
        panel.Controls.Add(buttons, 1, 0);
        return panel;
    }

    private Control BuildButtons()
    {
        var panel = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = true,
            FlowDirection = FlowDirection.RightToLeft,
            Margin = new Padding(0, 10, 0, 0)
        };
        panel.Controls.Add(_closeButton);
        panel.Controls.Add(_compileButton);
        return panel;
    }

    private static DataGridView CreateLevelGrid()
    {
        var grid = new DataGridView
        {
            Dock = DockStyle.Fill,
            AllowUserToAddRows = false,
            AllowUserToDeleteRows = false,
            AllowUserToResizeRows = false,
            AutoGenerateColumns = false,
            MultiSelect = false,
            ReadOnly = false,
            RowHeadersVisible = false,
            SelectionMode = DataGridViewSelectionMode.FullRowSelect,
            BackgroundColor = SystemColors.Window,
            BorderStyle = BorderStyle.Fixed3D
        };

        grid.Columns.Add(new DataGridViewCheckBoxColumn
        {
            HeaderText = "Select",
            Width = 58,
            SortMode = DataGridViewColumnSortMode.NotSortable
        });
        grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Level",
            Width = 260,
            ReadOnly = true,
            SortMode = DataGridViewColumnSortMode.NotSortable
        });
        grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Status",
            Width = 125,
            ReadOnly = true,
            SortMode = DataGridViewColumnSortMode.NotSortable
        });
        grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Progress",
            Width = 85,
            ReadOnly = true,
            SortMode = DataGridViewColumnSortMode.NotSortable
        });
        grid.Columns.Add(new DataGridViewTextBoxColumn
        {
            HeaderText = "Result",
            AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill,
            MinimumWidth = 220,
            ReadOnly = true,
            SortMode = DataGridViewColumnSortMode.NotSortable
        });

        return grid;
    }

    private void SubscribeEvents()
    {
        Shown += OnShown;
        _refreshButton.Click += OnRefreshClick;
        _selectAllButton.Click += OnSelectAllClick;
        _clearSelectionButton.Click += OnClearSelectionClick;
        _compileButton.Click += OnCompileClick;
        _levels.CellValueChanged += OnLevelCellValueChanged;
        _levels.CurrentCellDirtyStateChanged += OnLevelCurrentCellDirtyStateChanged;
        FormClosing += OnFormClosing;
    }

    private async void OnShown(object? sender, EventArgs eventArgs) =>
        await RefreshLevelsAsync();

    private async void OnRefreshClick(object? sender, EventArgs eventArgs) =>
        await RefreshLevelsAsync();

    private async Task RefreshLevelsAsync()
    {
        if (_compiling || _loadingLevels) return;
        _loadingLevels = true;
        _levels.Rows.Clear();
        _overallProgress.Value = 0;
        _status.Text = "Scanning DATA\\ENV\\PRODUCTION for levels...";
        _status.ForeColor = Color.DimGray;
        _productionPath.Text = string.Empty;
        UpdateActionAvailability();

        try
        {
            var settings = _settingsStore.Load();
            if (!GameInstallation.TryResolve(settings.GameDirectory, out var game, out var error))
                throw new InvalidOperationException(error);

            var production = WorldManifestLevelDiscovery.GetProductionDirectory(game.GameDirectory);
            _productionPath.Text = production;
            var discovered = await Task.Run(() =>
                WorldManifestLevelDiscovery.Discover(game.GameDirectory));

            PopulateLevels(discovered);
            var validCount = discovered.Count(level => level.CanCompile);
            _status.Text = discovered.Count == 0
                ? "No level directories containing both RENDERABLE and WORLD were found."
                : $"Found {discovered.Count:N0} level(s); {validCount:N0} can generate a World Manifest.";
            _status.ForeColor = discovered.Count == 0 ? Color.DarkOrange : Color.DarkGreen;
            _diagnostics.Log(
                "World Manifest",
                $"Level discovery found {discovered.Count} level(s) under {production}; compilable={validCount}.");
        }
        catch (Exception exception)
        {
            _status.Text = "Unable to load levels from the configured game directory.";
            _status.ForeColor = Color.DarkRed;
            _selectionSummary.Text = "No levels loaded.";
            _diagnostics.Log("World Manifest", $"Level discovery failed: {exception.Message}");
            MessageBox.Show(
                this,
                $"The level list could not be loaded.{Environment.NewLine}{exception.Message}{Environment.NewLine}{Environment.NewLine}" +
                "Configure the Alien: Isolation game directory in Settings, then click Refresh Levels.",
                "World Manifest Compiler",
                MessageBoxButtons.OK,
                MessageBoxIcon.Warning);
        }
        finally
        {
            _loadingLevels = false;
            UpdateSelectionSummary();
            UpdateActionAvailability();
        }
    }

    private void PopulateLevels(IReadOnlyList<WorldManifestLevelSource> discovered)
    {
        _levels.Rows.Clear();
        foreach (var level in discovered)
        {
            var rowIndex = _levels.Rows.Add(
                false,
                level.DisplayName,
                level.CanCompile ? "Ready" : "Unavailable",
                level.CanCompile ? "0%" : "—",
                level.CanCompile ? string.Empty : level.ValidationMessage);
            var row = _levels.Rows[rowIndex];
            row.Tag = level;
            row.Cells[SelectedColumnIndex].ReadOnly = !level.CanCompile;
            row.Cells[LevelColumnIndex].ToolTipText = level.LevelDirectory;
            row.Cells[ResultColumnIndex].ToolTipText = level.LevelDirectory;
            if (!level.CanCompile) row.DefaultCellStyle.ForeColor = Color.DimGray;
        }
        UpdateSelectionSummary();
    }

    private void OnSelectAllClick(object? sender, EventArgs eventArgs)
    {
        if (_compiling || _loadingLevels) return;
        foreach (DataGridViewRow row in _levels.Rows)
        {
            if (row.Tag is WorldManifestLevelSource { CanCompile: true })
                row.Cells[SelectedColumnIndex].Value = true;
        }
        UpdateSelectionSummary();
    }

    private void OnClearSelectionClick(object? sender, EventArgs eventArgs)
    {
        if (_compiling || _loadingLevels) return;
        foreach (DataGridViewRow row in _levels.Rows)
        {
            if (!row.Cells[SelectedColumnIndex].ReadOnly)
                row.Cells[SelectedColumnIndex].Value = false;
        }
        UpdateSelectionSummary();
    }

    private void OnLevelCurrentCellDirtyStateChanged(object? sender, EventArgs eventArgs)
    {
        if (_levels.IsCurrentCellDirty
            && _levels.CurrentCell?.ColumnIndex == SelectedColumnIndex)
            _levels.CommitEdit(DataGridViewDataErrorContexts.Commit);
    }

    private void OnLevelCellValueChanged(object? sender, DataGridViewCellEventArgs eventArgs)
    {
        if (eventArgs.RowIndex < 0 || eventArgs.ColumnIndex != SelectedColumnIndex) return;
        UpdateSelectionSummary();
        UpdateActionAvailability();
    }

    private async void OnCompileClick(object? sender, EventArgs eventArgs)
    {
        if (_compiling || _loadingLevels) return;
        _levels.EndEdit();
        var selected = GetSelectedRows();
        if (selected.Count == 0)
        {
            MessageBox.Show(
                this,
                "Select at least one level first.",
                "World Manifest Compiler",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        SetCompiling(true);
        _overallProgress.Value = 0;
        ResetSelectedRows(selected);

        var completed = 0;
        var created = 0;
        var cacheHits = 0;
        var failed = 0;

        foreach (var (row, level) in selected)
        {
            SetRowState(row, "Compiling", 0, string.Empty, Color.Black);
            _status.Text = $"Generating {completed + 1:N0} of {selected.Count:N0}: {level.DisplayName}";
            _status.ForeColor = Color.DimGray;

            var completedBeforeThisLevel = completed;
            var progress = new Progress<WorldManifestCompilerProgress>(value =>
            {
                SetRowState(row, value.Stage, value.Percentage, string.Empty, Color.Black);
                UpdateOverallProgress(completedBeforeThisLevel, selected.Count, value.Percentage);
                _status.Text = $"{level.DisplayName}: {value.Stage} ({value.Percentage}%)";
            });

            try
            {
                var result = await Task.Run(() =>
                    new WorldManifestCompiler().Compile(
                        level.LevelDirectory,
                        outputPath: null,
                        jsonPath: null,
                        useCache: true,
                        progress: progress));

                if (result.CacheHit) cacheHits++;
                else created++;

                var manifest = result.Manifest;
                SetRowState(
                    row,
                    result.CacheHit ? "Ready (cache)" : "Created",
                    100,
                    $"{manifest.MapContentId} — {manifest.Entities.Count:N0} entities",
                    Color.DarkGreen);
                row.Cells[ResultColumnIndex].ToolTipText = result.BinaryPath;
                _diagnostics.Log(
                    "World Manifest",
                    $"Batch {(result.CacheHit ? "cache hit" : "compiled")} for {level.DisplayName}; " +
                    $"map={manifest.MapContentId}; output={result.BinaryPath}; entities={manifest.Entities.Count}.");
            }
            catch (Exception exception)
            {
                failed++;
                SetRowState(row, "Failed", 0, exception.Message, Color.DarkRed);
                row.Cells[ResultColumnIndex].ToolTipText = exception.ToString();
                _diagnostics.Log(
                    "World Manifest",
                    $"Batch compilation failed for {level.DisplayName}: {exception.Message}");
            }

            completed++;
            UpdateOverallProgress(completed, selected.Count, 0);
        }

        _overallProgress.Value = 100;
        _status.Text = failed == 0
            ? $"Finished {selected.Count:N0} level(s): {created:N0} created, {cacheHits:N0} valid cache hit(s)."
            : $"Finished {selected.Count:N0} level(s): {created:N0} created, {cacheHits:N0} cache hit(s), {failed:N0} failed.";
        _status.ForeColor = failed == 0 ? Color.DarkGreen : Color.DarkOrange;
        SetCompiling(false);
    }

    private List<(DataGridViewRow Row, WorldManifestLevelSource Level)> GetSelectedRows()
    {
        var selected = new List<(DataGridViewRow, WorldManifestLevelSource)>();
        foreach (DataGridViewRow row in _levels.Rows)
        {
            if (row.Tag is not WorldManifestLevelSource { CanCompile: true } level) continue;
            if (row.Cells[SelectedColumnIndex].Value is true)
                selected.Add((row, level));
        }
        return selected;
    }

    private static void ResetSelectedRows(
        IEnumerable<(DataGridViewRow Row, WorldManifestLevelSource Level)> selected)
    {
        foreach (var (row, _) in selected)
            SetRowState(row, "Queued", 0, string.Empty, Color.Black);
    }

    private static void SetRowState(
        DataGridViewRow row,
        string status,
        int percentage,
        string result,
        Color foreground)
    {
        row.Cells[StatusColumnIndex].Value = status;
        row.Cells[ProgressColumnIndex].Value = $"{Math.Clamp(percentage, 0, 100)}%";
        row.Cells[ResultColumnIndex].Value = result;
        row.DefaultCellStyle.ForeColor = foreground;
    }

    private void UpdateOverallProgress(int completedLevels, int totalLevels, int currentPercentage)
    {
        if (totalLevels <= 0)
        {
            _overallProgress.Value = 0;
            return;
        }

        var aggregate = ((completedLevels * 100) + Math.Clamp(currentPercentage, 0, 100))
            / totalLevels;
        _overallProgress.Value = Math.Clamp(aggregate, 0, 100);
    }

    private void SetCompiling(bool compiling)
    {
        _compiling = compiling;
        _levels.Enabled = !compiling;
        _closeButton.Enabled = !compiling;
        UpdateActionAvailability();
    }

    private void UpdateSelectionSummary()
    {
        var total = _levels.Rows.Count;
        var compilable = 0;
        var selected = 0;
        foreach (DataGridViewRow row in _levels.Rows)
        {
            if (row.Tag is not WorldManifestLevelSource { CanCompile: true }) continue;
            compilable++;
            if (row.Cells[SelectedColumnIndex].Value is true) selected++;
        }

        _selectionSummary.Text = total == 0
            ? "No levels loaded."
            : $"{selected:N0} selected / {compilable:N0} compilable ({total:N0} discovered)";
    }

    private void UpdateActionAvailability()
    {
        var idle = !_compiling && !_loadingLevels;
        _refreshButton.Enabled = idle;
        _selectAllButton.Enabled = idle && _levels.Rows.Count != 0;
        _clearSelectionButton.Enabled = idle && _levels.Rows.Count != 0;
        _compileButton.Enabled = idle && HasSelectedCompilableLevel();
    }

    private bool HasSelectedCompilableLevel()
    {
        foreach (DataGridViewRow row in _levels.Rows)
        {
            if (row.Tag is WorldManifestLevelSource { CanCompile: true }
                && row.Cells[SelectedColumnIndex].Value is true)
                return true;
        }
        return false;
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs eventArgs)
    {
        if (!_compiling) return;
        eventArgs.Cancel = true;
        _status.Text = "Manifest generation is still running. Wait for it to finish before closing this window.";
        _status.ForeColor = Color.DarkOrange;
    }
}
