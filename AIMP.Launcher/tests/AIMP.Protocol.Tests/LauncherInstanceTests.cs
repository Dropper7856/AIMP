using System.Net;
using System.Net.Sockets;
using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

// Split-screen: one launcher per game, each in its own slot.
internal static class LauncherInstanceTests
{
    internal static Task SlotsAsync()
    {
        AssertEx.Equal(1, LauncherInstances.ReadSlot(null));
        AssertEx.Equal(1, LauncherInstances.ReadSlot("0"));
        AssertEx.Equal(1, LauncherInstances.ReadSlot("5"));
        AssertEx.Equal(1, LauncherInstances.ReadSlot("-2"));
        AssertEx.Equal(2, LauncherInstances.ReadSlot("2"));
        AssertEx.Equal(4, LauncherInstances.ReadSlot("4"));
        AssertEx.Equal("launcher-settings.json", LauncherInstances.SlotFileName("launcher-settings.json", 1));
        AssertEx.Equal("launcher-settings.instance-3.json", LauncherInstances.SlotFileName("launcher-settings.json", 3));
        AssertEx.Equal(5127, LauncherInstances.ApiPort(1));
        AssertEx.Equal(5128, LauncherInstances.ApiPort(2));
        var ports = Enumerable.Range(1, LauncherInstances.MaximumSlots)
            .SelectMany(slot => new[] { LauncherInstances.PrivateCommandPort(slot), LauncherInstances.PrivateEventPort(slot) })
            .ToArray();
        AssertEx.Equal(ports.Length, ports.Distinct().Count());
        AssertEx.False(ports.Contains(ScaleformBridgeListener.CommandPort) || ports.Contains(ScaleformBridgeListener.EventPort));
        return Task.CompletedTask;
    }

    internal static Task PlayerNamesAsync()
    {
        AssertEx.Equal("Ripley (2)", LauncherSettingsStore.InstanceName("Ripley", 2));
        var name = LauncherSettingsStore.InstanceName(new string('a', 200), 3);
        AssertEx.True(name.Length <= ProtocolConstants.MaximumPlayerNameLength);
        AssertEx.True(name.EndsWith(" (3)", StringComparison.Ordinal));
        AssertEx.True(PlayerName.TryNormalize(name, out _, out var error), error);
        return Task.CompletedTask;
    }

    // The public bridge routes a game's connection by the PID owning its socket.
    internal static async Task ConnectionOwnerAsync()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        using var client = new TcpClient();
        await client.ConnectAsync(IPAddress.Loopback, port);
        using var accepted = await listener.AcceptTcpClientAsync();
        var owner = TcpConnectionOwner.Find((IPEndPoint)accepted.Client.RemoteEndPoint!, port);
        AssertEx.Equal(Environment.ProcessId, owner ?? 0);
        AssertEx.Equal(0, TcpConnectionOwner.Find(new IPEndPoint(IPAddress.Loopback, 1), port) ?? 0);
    }
}
