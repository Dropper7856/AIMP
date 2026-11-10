using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Text;
using System.Text.Json;

namespace AIMP.Launcher;

/// <summary>
/// Launcher side of the Steam helper (<see cref="SteamHostServer"/>). Every Steam call
/// runs in a child process started when a game menu is ready and stopped when
/// Alien: Isolation exits, so Steam stops showing Spacewar as running even while
/// the launcher stays open. Game traffic is unchanged: the helper bridges Steam
/// connections to the same loopback TCP ports the in-process relay used.
/// </summary>
internal sealed class SteamRelayProcess : ISteamRelay, ISteamLobbies
{
    private const string NotRunningMessage = "Steam relay is waiting for the game menu to be ready.";
    private static readonly TimeSpan StopGrace = TimeSpan.FromSeconds(3);
    private static readonly Encoding Utf8 = new UTF8Encoding(false);

    private readonly LauncherDiagnostics _diagnostics;
    private readonly object _gate = new();
    private readonly List<SteamLobbyInvite> _invites = new(); // guarded by _gate
    private Helper? _helper; // guarded by _gate
    private long _nextId;
    private bool _disposed;

    internal SteamRelayProcess(LauncherDiagnostics diagnostics)
    {
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
    }

    public event Action<string, string>? JoinRequested;

    public event Action<SteamLobbyInvite>? InviteReceived;

    public ulong LocalSteamId { get; private set; }

    private sealed class Helper(Process process, int gameProcessId)
    {
        internal Process Process { get; } = process;
        internal int GameProcessId { get; } = gameProcessId;
        internal ConcurrentDictionary<long, TaskCompletionSource<SteamHostMessage>> Pending { get; } = new();
    }

    // ---- lifetime -----------------------------------------------------------

    public Task<bool> ActivateForMenuAsync(int gameProcessId)
    {
        if (gameProcessId <= 0) throw new ArgumentOutOfRangeException(nameof(gameProcessId));
        Helper? stale;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            stale = _helper is { } current && (current.Process.HasExited || current.GameProcessId != gameProcessId)
                ? DetachLocked() : null;
            _helper ??= StartHelper(gameProcessId);
        }
        if (stale is not null) _ = StopAsync(stale);
        return CallAsync<bool>("activate", gameProcessId);
    }

    /// <summary>Ends the helper process; the next ready game menu starts a new one.</summary>
    public async Task ReleaseAfterGameExitAsync()
    {
        Helper? helper;
        lock (_gate) helper = DetachLocked();
        if (helper is null) return;
        _diagnostics.Log("Steam", $"Alien: Isolation PID={helper.GameProcessId} exited; stopping the Steam helper so Steam no longer shows Spacewar as running.");
        await StopAsync(helper).ConfigureAwait(false);
    }

    public void Dispose()
    {
        Helper? helper;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            helper = DetachLocked();
        }
        if (helper is not null) StopAsync(helper).GetAwaiter().GetResult();
    }

    // Caller holds _gate. Asks the helper to shut Steam down; it exits on its own.
    private Helper? DetachLocked()
    {
        var helper = _helper;
        _helper = null;
        if (helper is null) return null;
        try
        {
            helper.Process.StandardInput.WriteLine(SteamHostProtocol.Serialize(new SteamHostMessage { Op = "shutdown" }));
            helper.Process.StandardInput.Close();
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException or InvalidOperationException) { }
        return helper;
    }

    private async Task StopAsync(Helper helper)
    {
        using var grace = new CancellationTokenSource(StopGrace);
        try { await helper.Process.WaitForExitAsync(grace.Token).ConfigureAwait(false); }
        catch (OperationCanceledException)
        {
            _diagnostics.Log("Steam", $"Steam helper PID={helper.Process.Id} did not exit in time; terminating it.");
            try { helper.Process.Kill(entireProcessTree: true); }
            catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception) { }
        }
        FailPending(helper, "The Steam helper stopped.");
        helper.Process.Dispose();
    }

    private Helper StartHelper(int gameProcessId)
    {
        var info = CreateStartInfo();
        var process = Process.Start(info) ?? throw new InvalidOperationException("Could not start the Steam helper.");
        var helper = new Helper(process, gameProcessId);
        _ = ReadAsync(helper);
        _diagnostics.Log("Steam", $"Started Steam helper PID={process.Id} for Alien: Isolation PID={gameProcessId}.");
        return helper;
    }

    private static ProcessStartInfo CreateStartInfo()
    {
        var (executable, prefix) = ResolveLauncherCommand();
        var info = new ProcessStartInfo(executable)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            StandardInputEncoding = Utf8,
            StandardOutputEncoding = Utf8,
            WorkingDirectory = AppContext.BaseDirectory
        };
        if (prefix is not null) info.ArgumentList.Add(prefix);
        info.ArgumentList.Add(SteamHostProtocol.Argument);
        info.ArgumentList.Add(Environment.ProcessId.ToString(CultureInfo.InvariantCulture));
        return info;
    }

    // The published launcher is an apphost exe; under "dotnet AIMP.Launcher.dll" pass the dll too.
    private static (string Executable, string? Prefix) ResolveLauncherCommand()
    {
        var path = Environment.ProcessPath ?? throw new InvalidOperationException("The launcher path is unknown.");
        var isDotnetHost = string.Equals(Path.GetFileNameWithoutExtension(path), "dotnet", StringComparison.OrdinalIgnoreCase);
        return isDotnetHost ? (path, Assembly.GetEntryAssembly()?.Location) : (path, null);
    }

    // ---- pipe ---------------------------------------------------------------

    private async Task ReadAsync(Helper helper)
    {
        try
        {
            while (await helper.Process.StandardOutput.ReadLineAsync().ConfigureAwait(false) is { } line)
                Dispatch(helper, line);
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException or InvalidOperationException) { }
        OnHelperOutputEnded(helper);
    }

    private void OnHelperOutputEnded(Helper helper)
    {
        FailPending(helper, "The Steam helper exited.");
        bool crashed;
        lock (_gate)
        {
            crashed = ReferenceEquals(_helper, helper);
            if (crashed) _helper = null;
        }
        if (!crashed) return; // a detached helper is disposed by StopAsync
        _diagnostics.Log("Steam", "The Steam helper exited unexpectedly; the next ready game menu restarts it.");
        helper.Process.Dispose();
    }

    private void Dispatch(Helper helper, string line)
    {
        SteamHostMessage message;
        try { message = SteamHostProtocol.Parse(line); }
        catch (Exception exception) when (exception is JsonException or InvalidDataException)
        {
            _diagnostics.Log("Steam", $"Ignored a malformed Steam helper message: {exception.Message}");
            return;
        }
        if (message.Event is not null) { HandleEvent(message); return; }
        if (message.SteamId != 0) LocalSteamId = message.SteamId;
        if (helper.Pending.TryRemove(message.Id, out var completion)) completion.TrySetResult(message);
    }

    private void HandleEvent(SteamHostMessage message)
    {
        try
        {
            switch (message.Event)
            {
                case "log": WithPair(message, (category, text) => _diagnostics.Log(category, text)); break;
                case "join": WithPair(message, RaiseJoin); break;
                case "invite": AddInvite(SteamHostProtocol.Read<SteamLobbyInvite>(message.Data)); break;
            }
        }
        catch (Exception exception) when (exception is JsonException or InvalidDataException)
        {
            _diagnostics.Log("Steam", $"Ignored a malformed Steam helper event: {exception.Message}");
        }
    }

    private static void WithPair(SteamHostMessage message, Action<string, string> action)
    {
        var pair = SteamHostProtocol.Read<string[]>(message.Data);
        if (pair.Length == 2) action(pair[0], pair[1]);
    }

    private void RaiseJoin(string friendName, string address)
    {
        var handler = JoinRequested;
        if (handler is not null) Task.Run(() => handler(friendName, address));
    }

    private static void FailPending(Helper helper, string reason)
    {
        foreach (var id in helper.Pending.Keys)
            if (helper.Pending.TryRemove(id, out var completion))
                completion.TrySetException(new IOException(reason));
    }

    private async Task<T> CallAsync<T>(string op, object? data = null)
    {
        var reply = await SendAsync(op, data).ConfigureAwait(false);
        if (!reply.Ok) throw SteamHostProtocol.ToException(reply);
        return reply.Data is { ValueKind: not JsonValueKind.Null } ? SteamHostProtocol.Read<T>(reply.Data) : default!;
    }

    private Task CallAsync(string op, object? data = null) => CallAsync<object?>(op, data);

    private Task<SteamHostMessage> SendAsync(string op, object? data)
    {
        var completion = new TaskCompletionSource<SteamHostMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_helper is not { } helper || helper.Process.HasExited)
                return Task.FromException<SteamHostMessage>(new InvalidOperationException(NotRunningMessage));
            var message = new SteamHostMessage { Id = ++_nextId, Op = op, Data = data is null ? null : SteamHostProtocol.ToElement(data) };
            helper.Pending[message.Id] = completion;
            if (!TryWriteLocked(helper, message, out var failure))
            {
                helper.Pending.TryRemove(message.Id, out _);
                return Task.FromException<SteamHostMessage>(failure!);
            }
        }
        return completion.Task;
    }

    // Caller holds _gate, which serializes writes to the helper's stdin.
    private static bool TryWriteLocked(Helper helper, SteamHostMessage message, out Exception? failure)
    {
        failure = null;
        try { helper.Process.StandardInput.WriteLine(SteamHostProtocol.Serialize(message)); }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException or InvalidOperationException)
        {
            failure = new IOException("The Steam helper is not running.", exception);
        }
        return failure is null;
    }

    private bool IsRunning
    {
        get { lock (_gate) return _helper is { Process.HasExited: false }; }
    }

    // ---- ISteamRelay --------------------------------------------------------

    public Task<bool> StartHostingAsync(int lobbyPort, string statusText) =>
        CallAsync<bool>("startHosting", new SteamHostHosting(lobbyPort, statusText, SteamLobbyVisibility.Public, null));

    // Nothing is hosted without a helper.
    public Task StopHostingAsync() => IsRunning ? CallAsync("stopHosting") : Task.CompletedTask;

    public Task<int> OpenClientTunnelAsync(ulong hostSteamId, CancellationToken cancellationToken) =>
        CallAsync<int>("openTunnel", hostSteamId).WaitAsync(cancellationToken);

    public void CloseAllTunnels(string reason)
    {
        if (!IsRunning) return;
        CallAsync("closeTunnels", reason).ContinueWith(task => _ = task.Exception, TaskContinuationOptions.OnlyOnFaulted);
    }

    // ---- ISteamLobbies ------------------------------------------------------

    public Task<bool> StartLobbyHostingAsync(int lobbyPort, SteamLobbyVisibility visibility, SteamLobbyAdvert advert) =>
        CallAsync<bool>("startHosting", new SteamHostHosting(lobbyPort, "Hosting an AIMP lobby", visibility, advert));

    public Task<bool> UpdateHostedLobbyAsync(SteamLobbyAdvert advert) =>
        IsRunning ? CallAsync<bool>("updateLobby", advert) : Task.FromResult(false);

    public Task<ulong> JoinLobbyAsync(ulong lobbyId) => CallAsync<ulong>("joinLobby", lobbyId);

    public async Task<IReadOnlyList<SteamLobbyListing>> FindLobbiesAsync() =>
        await CallAsync<SteamLobbyListing[]>("findLobbies").ConfigureAwait(false) ?? Array.Empty<SteamLobbyListing>();

    public async Task<IReadOnlyList<SteamFriendEntry>> GetFriendsAsync() =>
        await CallAsync<SteamFriendEntry[]>("friends").ConfigureAwait(false) ?? Array.Empty<SteamFriendEntry>();

    public Task<ulong> GetFriendLobbyAsync(ulong friendId) =>
        IsRunning ? CallAsync<ulong>("friendLobby", friendId) : Task.FromResult(0UL);

    public Task<string> InviteFriendAsync(ulong friendId) => CallAsync<string>("inviteFriend", friendId);

    // Invites are kept here so they survive a helper restart between games (as before).
    public IReadOnlyList<SteamLobbyInvite> GetInvites()
    {
        lock (_gate)
        {
            _invites.RemoveAll(invite => DateTime.UtcNow - invite.ReceivedUtc > SteamRelay.InviteLifetime);
            return _invites.ToArray();
        }
    }

    public SteamLobbyInvite? TakeInvite(ulong fromSteamId)
    {
        lock (_gate)
        {
            var invite = _invites.FirstOrDefault(value => value.FromSteamId == fromSteamId);
            if (invite is not null) _invites.Remove(invite);
            return invite;
        }
    }

    // The helper already drops repeats; keep its newest-first, one-per-friend, capped list.
    private void AddInvite(SteamLobbyInvite invite)
    {
        lock (_gate)
        {
            _invites.RemoveAll(value => value.FromSteamId == invite.FromSteamId);
            _invites.Insert(0, invite);
            if (_invites.Count > SteamRelay.MaxInvites)
                _invites.RemoveRange(SteamRelay.MaxInvites, _invites.Count - SteamRelay.MaxInvites);
        }
        var handler = InviteReceived;
        if (handler is not null) Task.Run(() => handler(invite));
    }
}
