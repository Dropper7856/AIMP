using System.Globalization;
using System.Text.Json;
using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private const string FirstCampaignLevel = "BSP_TORRENS";
    private const int MaximumOfferedCampaignSaves = 20;
    private static readonly string[] CampaignDifficulties = ["novice", "easy", "medium", "hard", "nightmare"];

    private sealed record CampaignProfileStatus(bool Ready, bool Busy, string SessionId, string ProfilePath,
        ulong SaveSequence, ulong WriteRevision, string Error);

    private CampaignSaveStore? _campaignStore;
    private byte[]? _hostCampaignBundle;
    // Session whose isolated native profile is configured on this PC.
    private Guid _campaignProfileSession;
    private string? _campaignProfilePath;
    private bool _campaignProfileReleasePending;
    private bool _campaignCaptureRunning;
    private DateTimeOffset _nextCampaignCaptureUtc;
    private ulong _campaignArchivedSequence;

    private CampaignSaveStore CampaignStore => _campaignStore ??= new CampaignSaveStore(
        Path.Combine(LauncherResetFiles.UserDataDirectory(), "CampaignSaves"));

    private string CampaignProfileDirectory(Guid sessionId) =>
        Path.Combine(LauncherResetFiles.UserDataDirectory(), "CampaignSaves", "Profiles", sessionId.ToString("N"));

    private Dictionary<string, string> AddCampaignSaves(Dictionary<string, string> values)
    {
        IReadOnlyList<CampaignSaveMetadata> saves;
        try { saves = CampaignStore.List(); }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            _diagnostics.Log("CampaignSaves", $"Campaign saves are unavailable: {exception.Message}");
            saves = [];
        }
        var offered = saves.Take(MaximumOfferedCampaignSaves).ToArray();
        values["campaignSaveCount"] = offered.Length.ToString(CultureInfo.InvariantCulture);
        for (var i = 0; i < offered.Length; i++)
        {
            values[$"campaignSave{i}Id"] = offered[i].Id;
            values[$"campaignSave{i}Label"] = offered[i].Label;
            values[$"campaignSave{i}Level"] = offered[i].Level;
        }
        return values;
    }

    // Campaign lobbies always start from the native campaign entry points: a new
    // game (first mission) or the host's archived checkpoint. The level shown in
    // the lobby is derived from that source, never chosen by the menu.
    private (ScaleformBridgeMessage Selection, CampaignLaunchDescriptor? Campaign, byte[]? Bundle)
        ResolveCampaignLobby(ScaleformBridgeMessage message)
    {
        if (!message.Get("mode", "survival").Equals("campaign", StringComparison.OrdinalIgnoreCase))
            return (message, null, null);
        var difficulty = message.Get("campaignDifficulty", "medium").ToLowerInvariant();
        if (!CampaignDifficulties.Contains(difficulty)) throw new InvalidOperationException("Invalid campaign difficulty.");
        string level;
        CampaignLaunchDescriptor campaign;
        byte[]? bundle = null;
        switch (message.Get("campaignAction", "new"))
        {
            case "new":
                level = FirstCampaignLevel;
                campaign = new("new", null, Guid.NewGuid().ToString("N"), difficulty, "New campaign");
                break;
            case "continue":
                var save = CampaignStore.Read(message.Get("campaignSaveId"));
                level = save.Metadata.Level;
                campaign = new("continue", save.Metadata.Id, save.Metadata.CampaignId, save.Metadata.Difficulty,
                    save.Metadata.Label, save.Metadata.Sha256, save.Metadata.ByteLength);
                bundle = save.Bytes;
                break;
            case "level":
                level = ResolveLevel(message);
                campaign = new("level", null, Guid.NewGuid().ToString("N"), difficulty, level);
                break;
            default:
                throw new InvalidOperationException("Invalid campaign action.");
        }
        CampaignSaveValidation.Validate(campaign);
        var values = new Dictionary<string, string>(message.Values, StringComparer.Ordinal)
        {
            ["mapId"] = level,
            ["mapName"] = level
        };
        return (message with { Values = values }, campaign, bundle);
    }

    // Preflight step: every peer runs the session in its own isolated native
    // profile. Continue launches stage the host's exact checkpoint bytes first.
    // Returns the bundle digest that the host requires in the ready report.
    private async Task<string> PrepareCampaignProfileAsync(LobbySnapshotPayload lobby, LobbyHost? host,
        LobbyClient? client, CancellationToken token)
    {
        var campaign = lobby.Campaign ?? throw new InvalidOperationException("The campaign lobby has no launch descriptor.");
        lock (_stateGate)
            if (_campaignProfileSession == lobby.SessionId && _campaignProfilePath is not null)
                return campaign.Sha256 ?? string.Empty;
        await SendStatusAsync(campaign.Action == "continue" ? "Downloading campaign save..." : "Preparing campaign profile...")
            .ConfigureAwait(false);
        var directory = CampaignProfileDirectory(lobby.SessionId);
        DeleteStaleCampaignProfiles(directory);
        if (Directory.Exists(directory)) Directory.Delete(directory, true);
        if (campaign.Action == "continue")
        {
            var bytes = host is not null
                ? _hostCampaignBundle ?? throw new InvalidOperationException("The selected checkpoint is unavailable.")
                : await client!.DownloadCampaignSaveAsync(campaign, token).ConfigureAwait(false);
            CampaignSaveValidation.ValidateBundle(campaign, bytes);
            CampaignStore.Stage(bytes, directory);
        }
        else Directory.CreateDirectory(directory);

        var status = await ExecuteCampaignProfileAsync("configure", directory, lobby.SessionId, token).ConfigureAwait(false);
        var deadline = Environment.TickCount64 + 15_000;
        while (!status.Ready)
        {
            if (Environment.TickCount64 >= deadline)
                throw new TimeoutException("The game did not reload the campaign save catalogue.");
            await Task.Delay(100, token).ConfigureAwait(false);
            status = await ExecuteCampaignProfileAsync("status", string.Empty, lobby.SessionId, token).ConfigureAwait(false);
        }
        if (!Guid.TryParse(status.SessionId, out var configured) || configured != lobby.SessionId)
            throw new InvalidOperationException("The game configured a different campaign profile.");
        lock (_stateGate)
        {
            _campaignProfileSession = lobby.SessionId;
            _campaignProfilePath = directory;
            _campaignProfileReleasePending = false;
            _campaignArchivedSequence = status.SaveSequence;
        }
        _diagnostics.Log("CampaignSaves", $"Campaign profile ready: action={campaign.Action}, level={lobby.Level}, save={campaign.SaveId}.");
        return campaign.Sha256 ?? string.Empty;
    }

    private void DeleteStaleCampaignProfiles(string current)
    {
        var root = Path.GetDirectoryName(current)!;
        if (!Directory.Exists(root)) return;
        foreach (var directory in Directory.EnumerateDirectories(root))
        {
            if (string.Equals(directory, current, StringComparison.OrdinalIgnoreCase)) continue;
            try
            {
                if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0) continue;
                Directory.Delete(directory, true);
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { }
        }
    }

    private async Task<CampaignProfileStatus> ExecuteCampaignProfileAsync(string action, string path, Guid sessionId,
        CancellationToken token)
    {
        var result = await _controller.ExecuteCommandAsync(GameCommandNames.CampaignProfile,
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["action"] = action,
                ["path"] = path,
                ["sessionId"] = sessionId.ToString("D")
            }, token).ConfigureAwait(false);
        CampaignProfileStatus? status = null;
        try { status = JsonSerializer.Deserialize<CampaignProfileStatus>(result.Message); }
        catch (JsonException) { }
        if (!result.Executed || status is null)
            throw new InvalidOperationException(status?.Error is { Length: > 0 } error ? error
                : $"Campaign profile {action} failed: {result.StatusCode} {result.Message}");
        return status;
    }

    // Host only: every successful native checkpoint becomes an immutable save
    // in the launcher store. Archives are accepted only when the profile was
    // idle and unchanged across the whole copy.
    private void TryScheduleCampaignSaveCapture(NativeRuntimeSnapshot runtime, DateTimeOffset now)
    {
        Guid session; string? path; bool release; CampaignLaunchDescriptor? campaign = null; string level = "";
        lock (_stateGate)
        {
            if (_campaignCaptureRunning || now < _nextCampaignCaptureUtc || _campaignProfilePath is null
                || !runtime.IpcConnected) return;
            release = _campaignProfileReleasePending;
            if (release)
            {
                if (!runtime.CurrentLevelName.EndsWith("Frontend", StringComparison.OrdinalIgnoreCase)
                    || runtime.LevelTransitioning) return;
            }
            else
            {
                if (_host is null || _start is null || _start.SessionId != _campaignProfileSession
                    || _start.Campaign is null) return;
                campaign = _start.Campaign;
                level = _start.Level;
            }
            session = _campaignProfileSession; path = _campaignProfilePath;
            _campaignCaptureRunning = true;
            _nextCampaignCaptureUtc = now.AddSeconds(3);
        }
        _ = release ? ClearCampaignProfileAsync(session) : CaptureCampaignSaveAsync(session, path, campaign!, level);
    }

    private async Task CaptureCampaignSaveAsync(Guid session, string path, CampaignLaunchDescriptor campaign, string level)
    {
        try
        {
            var before = await ExecuteCampaignProfileAsync("status", string.Empty, session, _lifetime.Token).ConfigureAwait(false);
            ulong archived;
            lock (_stateGate) archived = _campaignArchivedSequence;
            if (before.Busy || !before.Ready || before.SaveSequence <= archived) return;
            var snapshot = CampaignStore.CreateSnapshot(path, campaign.CampaignId, campaign.Difficulty, level);
            var after = await ExecuteCampaignProfileAsync("status", string.Empty, session, _lifetime.Token).ConfigureAwait(false);
            if (after.Busy || after.WriteRevision != before.WriteRevision)
            {
                CampaignStore.Delete(snapshot.Id);
                return; // A write overlapped the copy; the next poll retries.
            }
            lock (_stateGate) _campaignArchivedSequence = before.SaveSequence;
            _diagnostics.Log("CampaignSaves", $"Host checkpoint archived: {snapshot.Id} level={snapshot.Level}.");
            await SendStatusAsync($"Campaign saved ({snapshot.Level})").ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _diagnostics.Log("CampaignSaves", $"Campaign checkpoint archive failed: {exception.Message}");
        }
        finally { lock (_stateGate) _campaignCaptureRunning = false; }
    }

    private async Task ClearCampaignProfileAsync(Guid session)
    {
        try
        {
            await ExecuteCampaignProfileAsync("clear", string.Empty, session, _lifetime.Token).ConfigureAwait(false);
            lock (_stateGate)
            {
                if (_campaignProfileSession != session) return;
                _campaignProfileSession = Guid.Empty;
                _campaignProfilePath = null;
                _campaignProfileReleasePending = false;
            }
            _diagnostics.Log("CampaignSaves", "Personal saves restored.");
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _diagnostics.Log("CampaignSaves", $"Campaign profile release deferred: {exception.Message}");
        }
        finally { lock (_stateGate) _campaignCaptureRunning = false; }
    }

    // Leaving a session never un-isolates a running campaign world: the
    // personal saves are restored at the next frontend observation.
    private Task ReleaseCampaignProfileBestEffortAsync()
    {
        lock (_stateGate)
        {
            _hostCampaignBundle = null;
            if (_campaignProfilePath is not null) _campaignProfileReleasePending = true;
            _nextCampaignCaptureUtc = DateTimeOffset.MinValue;
        }
        return Task.CompletedTask;
    }
}
