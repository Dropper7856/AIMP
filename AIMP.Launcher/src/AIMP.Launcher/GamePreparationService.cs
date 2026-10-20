using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using AIMP.Launcher.Contracts;
using AIMP.Protocol;

namespace AIMP.Launcher;

// CathodeLib serializers and cache publication must not race debug compilation.
internal static class GamePreparationSynchronization
{
    internal static readonly object Gate = new();
}

public sealed class GamePreparationService
{
    private const int RecipeVersion = 2; // DLC-only preparation; invalidate campaign-inclusive receipts.
    private readonly object _sync = new();
    private readonly LauncherSettingsStore _settings;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly CancellationToken _shutdown;
    private Task _job = Task.CompletedTask;
    private PreparationReceipt? _receipt;
    private GamePreparationDto _status = EmptyStatus(string.Empty);

    internal GamePreparationService(LauncherSettingsStore settings, LauncherDiagnostics diagnostics,
        IHostApplicationLifetime lifetime)
    {
        _settings = settings;
        _diagnostics = diagnostics;
        _shutdown = lifetime.ApplicationStopping;
    }

    internal GamePreparationDto GetStatus()
    {
        var directory = _settings.Load().GameDirectory;
        lock (_sync)
            return SameDirectory(directory, _status.GameDirectory) ? _status : EmptyStatus(directory);
    }

    internal GamePreparationDto Start(bool revalidate = false)
    {
        var game = GameInstallation.Resolve(_settings.Load().GameDirectory);
        lock (_sync)
        {
            if (!_job.IsCompleted)
            {
                if (!SameDirectory(game.GameDirectory, _status.GameDirectory))
                    throw new InvalidOperationException("Another installation is being prepared. Wait for it to finish.");
                return _status;
            }
            if (!revalidate && SameDirectory(game.GameDirectory, _status.GameDirectory) && _status.State == "Ready")
                return _status;
            _receipt = null;
            _status = new(Guid.NewGuid().ToString("D"), game.GameDirectory, "Running", "Checking",
                0, 0, 0, string.Empty, "Checking your installation", string.Empty, DateTimeOffset.UtcNow, null);
            var id = _status.JobId;
            _job = Task.Run(() => Run(game.GameDirectory, id), CancellationToken.None);
            return _status;
        }
    }

    internal async Task EnsureReadyAsync(CancellationToken cancellationToken)
    {
        Start(revalidate: true);
        Task job;
        lock (_sync) job = _job;
        await job.WaitAsync(cancellationToken).ConfigureAwait(false);
        var state = GetStatus();
        if (state.State != "Ready")
            throw new InvalidOperationException(string.IsNullOrEmpty(state.Error) ? "Game preparation is not ready." : state.Error);
    }

    // This path is intentionally read-only. No imports or cold compilation after launch.
    internal Task<PreparedWorldManifest> PrepareLevelAsync(string level, CancellationToken cancellationToken) =>
        Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            PreparationReceipt receipt;
            var game = _settings.Load().GameDirectory;
            lock (_sync)
            {
                if (_status.State != "Ready" || !SameDirectory(game, _status.GameDirectory) || _receipt == null)
                    throw new InvalidOperationException("Prepare this installation in the Launcher before starting a multiplayer game.");
                receipt = _receipt;
            }
            var source = WorldManifestAutoResolver.ResolveLevelSource(game, level);
            var entry = receipt.Manifests.SingleOrDefault(m => SameDirectory(source, m.LevelDirectory))
                ?? throw new InvalidOperationException("This level has no prepared world manifest. Close the game and prepare the installation again.");
            var world = ResolveWorldDirectory(source);
            if (HashIfPresent(Path.Combine(world, "COMMANDS.PAK")) != entry.CommandsPakSha256
                || HashIfPresent(Path.Combine(world, "COMMANDS.BIN")) != entry.CommandsBinSha256)
                throw new InvalidOperationException("Level files changed since preparation. Close the game and prepare the installation again.");
            cancellationToken.ThrowIfCancellationRequested();
            var prepared = WorldManifestSessionLoader.Load(entry.ManifestPath);
            if (!string.Equals(prepared.BinarySha256, entry.BinarySha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The prepared world manifest changed. Close the game and prepare the installation again.");
            return prepared;
        }, cancellationToken);

    private void Run(string game, string jobId)
    {
        try
        {
            lock (GamePreparationSynchronization.Gate)
            {
                _shutdown.ThrowIfCancellationRequested();
                AssertGameStopped();
                var levels = WorldManifestLevelDiscovery.Discover(game).Where(l => l.CanCompile).ToArray();
                if (levels.Length == 0) throw new InvalidDataException("No playable levels were found.");
                var receiptPath = ReceiptPath(game);
                var existing = TryReadReceipt(receiptPath);
                if (existing != null && existing.RecipeVersion == RecipeVersion
                    && existing.CompilerVersion == WorldManifestConstants.CompilerVersion
                    && SameDirectory(existing.GameDirectory, game)
                    && existing.Manifests.Length == levels.Length
                    && existing.Manifests.All(m => levels.Any(l => SameDirectory(l.LevelDirectory, m.LevelDirectory)))
                    && StampsEqual(existing.Files, CaptureStamps(game, levels, existing.Manifests)))
                {
                    Complete(existing);
                    return;
                }
                Update("Character", 2, 0, levels.Length, string.Empty, "Preparing multiplayer characters");
                RipleyCharacterInstaller.Install(game, new InlineProgress<RipleyInstallProgress>(p =>
                    Update("Character", 2 + (int)(38L * p.CompletedLevels / Math.Max(1, p.TotalLevels)),
                        p.CompletedLevels, p.TotalLevels, p.Level, p.Message)), _shutdown);
                var manifests = new List<PreparedManifestReceipt>();
                for (var i = 0; i < levels.Length; i++)
                {
                    _shutdown.ThrowIfCancellationRequested();
                    AssertGameStopped();
                    var index = i;
                    var level = levels[i];
                    var result = new WorldManifestCompiler().Compile(level.LevelDirectory, progress:
                        new InlineProgress<WorldManifestCompilerProgress>(p => Update("Manifests",
                            40 + (int)(60L * (index * 100L + p.Percentage) / (levels.Length * 100L)),
                            index, levels.Length, level.DisplayName, "Preparing level data")));
                    // Validate the binary even for non-playable/cinematic levels with no World probe.
                    var manifest = WorldManifestBinaryCodec.Read(result.BinaryPath);
                    if (manifest.CompilerVersion != WorldManifestConstants.CompilerVersion)
                        throw new InvalidDataException("A world manifest was produced by an incompatible compiler.");
                    var world = ResolveWorldDirectory(level.LevelDirectory);
                    var pakHash = HashIfPresent(Path.Combine(world, "COMMANDS.PAK"));
                    var binHash = HashIfPresent(Path.Combine(world, "COMMANDS.BIN"));
                    if (!string.Equals(manifest.Source.CommandsPakSha256, pakHash, StringComparison.OrdinalIgnoreCase)
                        || !string.Equals(manifest.Source.CommandsBinSha256, binHash, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Level files changed while preparing a manifest. Retry preparation.");
                    manifests.Add(new(level.LevelDirectory, result.BinaryPath,
                        WorldReplicationCodec.ComputeManifestSha256(result.BinaryPath),
                        pakHash, binHash));
                }
                var array = manifests.ToArray();
                var receipt = new PreparationReceipt(RecipeVersion, WorldManifestConstants.CompilerVersion,
                    game, CaptureStamps(game, levels, array), array);
                Directory.CreateDirectory(Path.GetDirectoryName(receiptPath)!);
                var temporary = receiptPath + ".tmp";
                File.WriteAllText(temporary, JsonSerializer.Serialize(receipt));
                File.Move(temporary, receiptPath, true);
                Complete(receipt);
            }
        }
        catch (Exception error)
        {
            _diagnostics.Log("Preparation", $"Preparation {jobId} failed: {error}");
            lock (_sync) _status = _status with
            {
                State = error is OperationCanceledException ? "Canceled" : "Failed",
                Message = "Game preparation could not finish", Error = error.Message,
                CompletedAtUtc = DateTimeOffset.UtcNow
            };
        }
    }

    private void Complete(PreparationReceipt receipt)
    {
        lock (_sync)
        {
            _receipt = receipt;
            _status = _status with { State = "Ready", Stage = "Ready", Percentage = 100,
                CompletedLevels = receipt.Manifests.Length, TotalLevels = receipt.Manifests.Length,
                CurrentLevel = string.Empty, Message = "Ready to play", Error = string.Empty,
                CompletedAtUtc = DateTimeOffset.UtcNow };
        }
        _diagnostics.Log("Preparation", $"Installation ready: {receipt.Manifests.Length} level manifests verified.");
    }

    private void Update(string stage, int percentage, int completed, int total, string level, string message)
    {
        lock (_sync) _status = _status with { Stage = stage, Percentage = percentage,
            CompletedLevels = completed, TotalLevels = total, CurrentLevel = level, Message = message };
    }

    private static Dictionary<string, FileStamp> CaptureStamps(string game,
        IReadOnlyList<WorldManifestLevelSource> levels, IReadOnlyList<PreparedManifestReceipt> manifests)
    {
        var paths = levels.SelectMany(l => new[] { l.LevelDirectory, l.LevelDirectory + "_PATCH" })
            .Append(Path.Combine(game, "DATA", "ENV", "GLOBAL"))
            .Where(Directory.Exists)
            .SelectMany(root => Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
                .Where(p => p.EndsWith(".PAK", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".BIN", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".MTL", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".MAP", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".DAT", StringComparison.OrdinalIgnoreCase)))
            .Concat(manifests.Select(m => m.ManifestPath))
            .Append(Path.Combine(game, "DATA", "GLOBAL", "ANIMATION.PAK"));
        return paths.Distinct(StringComparer.OrdinalIgnoreCase).ToDictionary(p => Path.GetFullPath(p), p =>
        {
            var info = new FileInfo(p);
            if (!info.Exists) throw new FileNotFoundException("A prepared asset is missing.", p);
            return new FileStamp(info.Length, info.LastWriteTimeUtc.Ticks);
        }, StringComparer.OrdinalIgnoreCase);
    }

    private static bool StampsEqual(Dictionary<string, FileStamp> a, Dictionary<string, FileStamp> b) =>
        a.Count == b.Count && b.All(pair => a.TryGetValue(pair.Key, out var value) && value == pair.Value);
    private static PreparationReceipt? TryReadReceipt(string path)
    {
        try
        {
            var receipt = File.Exists(path) ? JsonSerializer.Deserialize<PreparationReceipt>(File.ReadAllText(path)) : null;
            return receipt is { Files: not null, Manifests: not null, GameDirectory: not null }
                && receipt.Manifests.All(m => m is not null && !string.IsNullOrWhiteSpace(m.ManifestPath)
                    && !string.IsNullOrWhiteSpace(m.LevelDirectory)) ? receipt : null;
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException) { return null; }
    }
    private static string ReceiptPath(string game) => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AIMP", "Preparation",
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(game).ToUpperInvariant()))) + ".json");
    private static string ResolveWorldDirectory(string level) => WorldManifestCompiler.ResolveWorldDirectory(level);
    private static string HashIfPresent(string path)
    {
        if (!File.Exists(path)) return string.Empty;
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }
    private static bool SameDirectory(string a, string b) => string.Equals(
        a.TrimEnd('\\', '/'), b.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase);
    private static void AssertGameStopped()
    {
        var processes = Process.GetProcessesByName("AI");
        try
        {
            if (processes.Any(p => !p.HasExited))
                throw new InvalidOperationException("Close Alien: Isolation before preparing its files.");
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }
    private static GamePreparationDto EmptyStatus(string directory) => new(string.Empty, directory,
        string.IsNullOrWhiteSpace(directory) ? "NotConfigured" : "Idle", string.Empty, 0, 0, 0,
        string.Empty, "Select your game installation", string.Empty, null, null);
    private sealed record FileStamp(long Length, long LastWriteTicks);
    private sealed record PreparedManifestReceipt(string LevelDirectory, string ManifestPath,
        string BinarySha256, string CommandsPakSha256, string CommandsBinSha256);
    private sealed record PreparationReceipt(int RecipeVersion, uint CompilerVersion, string GameDirectory,
        Dictionary<string, FileStamp> Files, PreparedManifestReceipt[] Manifests);
    private sealed class InlineProgress<T>(Action<T> report) : IProgress<T>
    {
        public void Report(T value) => report(value);
    }
}
