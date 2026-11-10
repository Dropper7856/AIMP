using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace AIMP.Launcher;

/// <summary>
/// "AIMP.Launcher.exe --aimp-steam-host &lt;launcherPid&gt;": owns SteamClient (Spacewar 480)
/// in a child process. Steam keeps showing the player in-game for as long as the
/// process that initialized the Steam API lives (SteamAPI_Shutdown does not clear
/// it), so the launcher ends this process when Alien: Isolation exits.
/// Exits on "shutdown", when stdin closes, or when the launcher process ends.
/// </summary>
internal sealed class SteamHostServer
{
    private readonly object _writeGate = new();
    private readonly TextWriter _output;
    private readonly SteamRelay _relay;
    private bool _outputBroken;

    private SteamHostServer(TextWriter output)
    {
        _output = output;
        _relay = new SteamRelay(new SteamRelayLog((category, message) => Notify("log", new[] { category, message })));
        _relay.JoinRequested += (friend, address) => Notify("join", new[] { friend, address });
        _relay.InviteReceived += invite => Notify("invite", invite);
    }

    internal static bool TryRun(string[] args, out int exitCode)
    {
        exitCode = 0;
        if (args.Length != 2 || !string.Equals(args[0], SteamHostProtocol.Argument, StringComparison.Ordinal)) return false;
        if (!int.TryParse(args[1], NumberStyles.None, CultureInfo.InvariantCulture, out var parentId) || parentId <= 0)
        {
            exitCode = 2;
            return true;
        }
        var output = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true };
        // stdout carries only protocol lines.
        Console.SetOut(TextWriter.Null);
        Console.SetError(TextWriter.Null);
        exitCode = new SteamHostServer(output).RunAsync(parentId).GetAwaiter().GetResult();
        return true;
    }

    private async Task<int> RunAsync(int parentId)
    {
        if (!WatchParent(parentId)) return 0;
        using var input = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false));
        try
        {
            while (await input.ReadLineAsync().ConfigureAwait(false) is { } line)
                if (!HandleLine(line)) break;
        }
        finally { _relay.Dispose(); }
        return 0;
    }

    // Exiting runs ProcessExit, where SteamRelay shuts Steam down.
    private static bool WatchParent(int parentId)
    {
        try
        {
            var parent = Process.GetProcessById(parentId);
            parent.EnableRaisingEvents = true;
            parent.Exited += (_, _) => Environment.Exit(0);
            return !parent.HasExited;
        }
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException
            or System.ComponentModel.Win32Exception)
        {
            return false;
        }
    }

    private bool HandleLine(string line)
    {
        if (string.IsNullOrWhiteSpace(line)) return true;
        SteamHostMessage request;
        try { request = SteamHostProtocol.Parse(line); }
        catch (Exception exception) when (exception is JsonException or InvalidDataException)
        {
            Notify("log", new[] { "Steam", $"Steam helper ignored a malformed request: {exception.Message}" });
            return true;
        }
        if (request.Op == "shutdown") return false;
        _ = DispatchAsync(request);
        return true;
    }

    private async Task DispatchAsync(SteamHostMessage request)
    {
        SteamHostMessage reply;
        try
        {
            var result = await ExecuteAsync(request.Op ?? string.Empty, request.Data).ConfigureAwait(false);
            reply = new SteamHostMessage { Id = request.Id, Ok = true, Data = result };
        }
        catch (Exception exception)
        {
            reply = new SteamHostMessage { Id = request.Id, Error = exception.Message, ErrorKind = SteamHostProtocol.KindOf(exception) };
        }
        Write(reply with { SteamId = _relay.LocalSteamId });
    }

    private Task<JsonElement?> ExecuteAsync(string op, JsonElement? data) => op switch
    {
        "activate" => Wrap(_relay.ActivateForMenuAsync(SteamHostProtocol.Read<int>(data))),
        "startHosting" => Wrap(StartHostingAsync(SteamHostProtocol.Read<SteamHostHosting>(data))),
        "stopHosting" => Done(_relay.StopHostingAsync()),
        "openTunnel" => Wrap(_relay.OpenClientTunnelAsync(SteamHostProtocol.Read<ulong>(data), CancellationToken.None)),
        "closeTunnels" => Done(CloseTunnels(SteamHostProtocol.Read<string>(data))),
        _ => ExecuteLobbyAsync(op, data)
    };

    private Task<JsonElement?> ExecuteLobbyAsync(string op, JsonElement? data) => op switch
    {
        "updateLobby" => Wrap(_relay.UpdateHostedLobbyAsync(SteamHostProtocol.Read<SteamLobbyAdvert>(data))),
        "joinLobby" => Wrap(_relay.JoinLobbyAsync(SteamHostProtocol.Read<ulong>(data))),
        "findLobbies" => Wrap(_relay.FindLobbiesAsync()),
        "friends" => Wrap(_relay.GetFriendsAsync()),
        "friendLobby" => Wrap(_relay.GetFriendLobbyAsync(SteamHostProtocol.Read<ulong>(data))),
        "inviteFriend" => Wrap(_relay.InviteFriendAsync(SteamHostProtocol.Read<ulong>(data))),
        _ => throw new InvalidDataException($"Unknown Steam helper operation '{op}'.")
    };

    // Without an advert this is the legacy relay-only hosting (no Steam lobby).
    private Task<bool> StartHostingAsync(SteamHostHosting hosting) => hosting.Advert is { } advert
        ? _relay.StartLobbyHostingAsync(hosting.Port, hosting.Visibility, advert)
        : _relay.StartHostingAsync(hosting.Port, hosting.Status);

    private Task CloseTunnels(string reason)
    {
        _relay.CloseAllTunnels(reason);
        return Task.CompletedTask;
    }

    private static async Task<JsonElement?> Wrap<T>(Task<T> task) =>
        SteamHostProtocol.ToElement((await task.ConfigureAwait(false))!);

    private static async Task<JsonElement?> Done(Task task)
    {
        await task.ConfigureAwait(false);
        return null;
    }

    private void Notify(string name, object value) =>
        Write(new SteamHostMessage { Event = name, Data = SteamHostProtocol.ToElement(value) });

    // A broken pipe means the launcher is gone; stdin EOF or the parent watch ends the process.
    private void Write(SteamHostMessage message)
    {
        var line = SteamHostProtocol.Serialize(message);
        lock (_writeGate)
        {
            if (_outputBroken) return;
            try { _output.WriteLine(line); }
            catch (Exception exception) when (exception is IOException or ObjectDisposedException) { _outputBroken = true; }
        }
    }
}
