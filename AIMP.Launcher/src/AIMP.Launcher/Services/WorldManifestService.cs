using System.Collections.Concurrent;
using AIMP.Launcher.Contracts;

namespace AIMP.Launcher.Services;

public sealed class WorldManifestService
{
    private const int MaximumRetainedJobs = 20;
    private readonly LauncherSettingsStore _settings;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly IHostApplicationLifetime _applicationLifetime;
    private readonly ConcurrentDictionary<Guid, CompilationJob> _jobs = new();
    private readonly object _startGate = new();
    private int _resetEpoch;

    internal WorldManifestService(
        LauncherSettingsStore settings,
        LauncherDiagnostics diagnostics,
        IHostApplicationLifetime applicationLifetime)
    {
        _settings = settings;
        _diagnostics = diagnostics;
        _applicationLifetime = applicationLifetime;
    }

    public Task<WorldLevelListResponse> DiscoverLevelsAsync(CancellationToken cancellationToken) =>
        Task.Run(DiscoverLevels, cancellationToken);

    public async Task<WorldManifestCompilationJobResponse> StartCompilationAsync(
        StartWorldManifestCompilationRequest request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.RelativePaths is not { Count: > 0 })
            throw new ArgumentException("Select at least one level.", nameof(request));

        var epoch = Volatile.Read(ref _resetEpoch);
        var gameDirectory = _settings.Load().GameDirectory;
        var available = await DiscoverSourcesAsync(gameDirectory, cancellationToken).ConfigureAwait(false);
        var byRelativePath = available.ToDictionary(
            level => level.RelativePath,
            StringComparer.OrdinalIgnoreCase);
        var selected = new List<WorldManifestLevelSource>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var relativePath in request.RelativePaths)
        {
            if (string.IsNullOrWhiteSpace(relativePath)
                || !byRelativePath.TryGetValue(relativePath.Trim(), out var source))
                throw new ArgumentException(
                    $"Level '{relativePath}' is not part of the configured game installation.",
                    nameof(request));
            if (!source.CanCompile)
                throw new ArgumentException(
                    $"Level '{source.RelativePath}' cannot be compiled: {source.ValidationMessage}",
                    nameof(request));
            if (seen.Add(source.RelativePath)) selected.Add(source);
        }

        CompilationJob job;
        lock (_startGate)
        {
            if (epoch != _resetEpoch)
                throw new InvalidOperationException("Launcher settings changed. Select the levels again.");
            if (_jobs.Values.Any(value => value.IsActive))
                throw new InvalidOperationException(
                    "A World Manifest compilation is already running.");
            PruneCompletedJobs();
            job = new CompilationJob(selected, gameDirectory, request.ImportAssets);
            if (!_jobs.TryAdd(job.Id, job))
                throw new InvalidOperationException("The compilation job could not be created.");
        }

        _ = Task.Run(() => RunCompilationAsync(job), CancellationToken.None);
        return job.Snapshot();
    }

    internal int Reset(Func<int> deleteData)
    {
        lock (_startGate)
        {
            if (_jobs.Values.Any(job => job.IsActive))
                throw new InvalidOperationException("Wait for manifest compilation to finish before resetting the launcher.");
            Interlocked.Increment(ref _resetEpoch);
            var deleted = deleteData();
            _jobs.Clear();
            return deleted;
        }
    }

    public WorldManifestCompilationJobResponse GetJob(Guid id) =>
        _jobs.TryGetValue(id, out var job)
            ? job.Snapshot()
            : throw new KeyNotFoundException($"World Manifest compilation job '{id}' was not found.");

    public IReadOnlyList<WorldManifestCompilationJobResponse> GetJobs() =>
        _jobs.Values
            .OrderByDescending(job => job.CreatedAtUtc)
            .Select(job => job.Snapshot())
            .ToArray();

    private WorldLevelListResponse DiscoverLevels()
    {
        var settings = _settings.Load();
        if (!GameInstallation.TryResolve(settings.GameDirectory, out var game, out var error))
            throw new InvalidOperationException(error);
        var production = WorldManifestLevelDiscovery.GetProductionDirectory(game.GameDirectory);
        var levels = WorldManifestLevelDiscovery.Discover(game.GameDirectory, includeCampaign: true);
        _diagnostics.Log(
            "World Manifest",
            $"Level discovery found {levels.Count} level(s) under {production}; " +
            $"compilable={levels.Count(level => level.CanCompile)}.");
        return new WorldLevelListResponse(
            production,
            levels.Select(MapLevel).ToArray());
    }

    private Task<IReadOnlyList<WorldManifestLevelSource>> DiscoverSourcesAsync(
        string gameDirectory, CancellationToken cancellationToken) => Task.Run(() =>
    {
        if (!GameInstallation.TryResolve(gameDirectory, out var game, out var error))
            throw new InvalidOperationException(error);
        return WorldManifestLevelDiscovery.Discover(game.GameDirectory, includeCampaign: true);
    }, cancellationToken);

    private Task RunCompilationAsync(CompilationJob job)
    {
        job.MarkRunning();
        try
        {
            for (var index = 0; index < job.Sources.Count; index++)
            {
                if (_applicationLifetime.ApplicationStopping.IsCancellationRequested)
                {
                    job.MarkCanceled();
                    return Task.CompletedTask;
                }

                var source = job.Sources[index];
                var itemIndex = index;
                job.UpdateItem(itemIndex, "Compiling", 0, "Preparing manifest compilation...");
                var progress = new InlineProgress<WorldManifestCompilerProgress>(value =>
                    job.UpdateItem(itemIndex, value.Stage, value.Percentage, value.Stage));
                try
                {
                    WorldManifestCompilationResult result;
                    lock (GamePreparationSynchronization.Gate)
                    {
                        if (job.ImportAssets)
                        {
                            job.UpdateItem(itemIndex, "Importing", 0, "Preparing E_RIPLEY character assets...");
                            RipleyCharacterInstaller.InstallLevel(job.GameDirectory, source.LevelDirectory,
                                new InlineProgress<RipleyInstallProgress>(value =>
                                    job.UpdateItem(itemIndex, "Importing", 0, value.Message)),
                                _applicationLifetime.ApplicationStopping);
                        }
                        result = new WorldManifestCompiler().Compile(source.LevelDirectory,
                            useCache: true, progress: progress);
                    }
                    job.CompleteItem(itemIndex, result);
                    _diagnostics.Log(
                        "World Manifest",
                        $"Batch {(result.CacheHit ? "cache hit" : "compiled")} for " +
                        $"{source.DisplayName}; map={result.Manifest.MapContentId}; " +
                        $"output={result.BinaryPath}; entities={result.Manifest.Entities.Count}.");
                }
                catch (OperationCanceledException) when (_applicationLifetime.ApplicationStopping.IsCancellationRequested)
                {
                    job.MarkCanceled();
                    return Task.CompletedTask;
                }
                catch (Exception exception)
                {
                    job.FailItem(itemIndex, exception.Message);
                    _diagnostics.Log(
                        "World Manifest",
                        $"Batch compilation failed for {source.DisplayName}: {exception.Message}");
                }
            }
            job.MarkCompleted();
        }
        catch (Exception exception)
        {
            job.MarkFailed(exception.Message);
            _diagnostics.Log(
                "World Manifest", $"Compilation job {job.Id} failed: {exception.Message}");
        }
        return Task.CompletedTask;
    }

    private void PruneCompletedJobs()
    {
        var excess = _jobs.Count - MaximumRetainedJobs + 1;
        if (excess <= 0) return;
        foreach (var job in _jobs.Values
                     .Where(value => !value.IsActive)
                     .OrderBy(value => value.CreatedAtUtc)
                     .Take(excess))
            _jobs.TryRemove(job.Id, out _);
    }

    private static WorldLevelResponse MapLevel(WorldManifestLevelSource value) => new(
        value.DisplayName,
        value.RelativePath,
        value.LevelDirectory,
        value.CanCompile,
        value.ValidationMessage);

    private sealed class CompilationJob
    {
        private readonly object _gate = new();
        private readonly MutableItem[] _items;
        private string _status = "Queued";
        private int _completedLevels;
        private int _created;
        private int _cacheHits;
        private int _failed;
        private DateTimeOffset? _completedAtUtc;

        internal CompilationJob(IReadOnlyList<WorldManifestLevelSource> sources, string gameDirectory, bool importAssets)
        {
            Sources = sources;
            GameDirectory = gameDirectory;
            ImportAssets = importAssets;
            _items = sources.Select(source => new MutableItem(source)).ToArray();
        }

        internal string GameDirectory { get; }
        internal bool ImportAssets { get; }
        internal Guid Id { get; } = Guid.NewGuid();
        internal DateTimeOffset CreatedAtUtc { get; } = DateTimeOffset.UtcNow;
        internal IReadOnlyList<WorldManifestLevelSource> Sources { get; }

        internal bool IsActive
        {
            get
            {
                lock (_gate) return _status is "Queued" or "Running";
            }
        }

        internal void MarkRunning()
        {
            lock (_gate) _status = "Running";
        }

        internal void UpdateItem(int index, string status, int percentage, string stage)
        {
            lock (_gate)
            {
                var item = _items[index];
                item.Status = status;
                item.Percentage = Math.Clamp(percentage, 0, 100);
                item.Stage = stage;
            }
        }

        internal void CompleteItem(int index, WorldManifestCompilationResult result)
        {
            lock (_gate)
            {
                var item = _items[index];
                item.Status = result.CacheHit ? "Ready (cache)" : "Created";
                item.Percentage = 100;
                item.Stage = "Manifest ready";
                item.MapContentId = result.Manifest.MapContentId.ToString();
                item.EntityCount = result.Manifest.Entities.Count;
                item.BinaryPath = result.BinaryPath;
                item.CacheHit = result.CacheHit;
                _completedLevels++;
                if (result.CacheHit) _cacheHits++;
                else _created++;
            }
        }

        internal void FailItem(int index, string error)
        {
            lock (_gate)
            {
                var item = _items[index];
                item.Status = "Failed";
                item.Percentage = 0;
                item.Stage = "Compilation failed";
                item.Error = error;
                _completedLevels++;
                _failed++;
            }
        }

        internal void MarkCompleted()
        {
            lock (_gate)
            {
                _status = _failed == 0 ? "Completed" : "CompletedWithErrors";
                _completedAtUtc = DateTimeOffset.UtcNow;
            }
        }

        internal void MarkCanceled()
        {
            lock (_gate)
            {
                _status = "Canceled";
                _completedAtUtc = DateTimeOffset.UtcNow;
            }
        }

        internal void MarkFailed(string error)
        {
            lock (_gate)
            {
                _status = "Failed";
                _completedAtUtc = DateTimeOffset.UtcNow;
                var pending = _items.FirstOrDefault(item => item.Error is null && item.Percentage < 100);
                if (pending is not null)
                {
                    pending.Status = "Failed";
                    pending.Stage = "Compilation job failed";
                    pending.Error = error;
                }
            }
        }

        internal WorldManifestCompilationJobResponse Snapshot()
        {
            lock (_gate)
            {
                var activePercentage = _items.Length == 0
                    ? 0
                    : ((_completedLevels * 100)
                        + _items.Where(item => item.Status is not "Created" and not "Ready (cache)" and not "Failed")
                            .Select(item => item.Percentage)
                            .DefaultIfEmpty(0)
                            .Max()) / _items.Length;
                var overallPercentage = _status is "Completed" or "CompletedWithErrors"
                    ? 100
                    : Math.Clamp(activePercentage, 0, 100);
                return new WorldManifestCompilationJobResponse(
                    Id,
                    _status,
                    overallPercentage,
                    _completedLevels,
                    _items.Length,
                    _created,
                    _cacheHits,
                    _failed,
                    CreatedAtUtc,
                    _completedAtUtc,
                    _items.Select(item => item.Snapshot()).ToArray());
            }
        }
    }

    private sealed class MutableItem
    {
        internal MutableItem(WorldManifestLevelSource source)
        {
            Source = source;
        }

        internal WorldManifestLevelSource Source { get; }
        internal string Status { get; set; } = "Queued";
        internal int Percentage { get; set; }
        internal string Stage { get; set; } = "Waiting";
        internal string? MapContentId { get; set; }
        internal int? EntityCount { get; set; }
        internal string? BinaryPath { get; set; }
        internal bool? CacheHit { get; set; }
        internal string? Error { get; set; }

        internal WorldManifestCompilationItemResponse Snapshot() => new(
            Source.RelativePath,
            Source.DisplayName,
            Status,
            Percentage,
            Stage,
            MapContentId,
            EntityCount,
            BinaryPath,
            CacheHit,
            Error);
    }

    private sealed class InlineProgress<T> : IProgress<T>
    {
        private readonly Action<T> _report;

        internal InlineProgress(Action<T> report)
        {
            _report = report;
        }

        public void Report(T value) => _report(value);
    }
}
