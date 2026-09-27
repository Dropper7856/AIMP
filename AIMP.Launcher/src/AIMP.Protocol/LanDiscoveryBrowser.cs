using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;

namespace AIMP.Protocol;

public sealed class LanDiscoveryBrowser : IAsyncDisposable
{
    private readonly ConcurrentDictionary<Guid, DiscoveredSession> _sessions = new();
    private readonly UdpClient _udp;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Task _receiveLoop;
    private readonly Task _expiryLoop;

    public LanDiscoveryBrowser(int discoveryPort = ProtocolConstants.DefaultDiscoveryPort)
    {
        if (discoveryPort is < 1 or > 65535)
        {
            throw new ArgumentOutOfRangeException(nameof(discoveryPort));
        }

        _udp = new UdpClient(AddressFamily.InterNetwork);
        _udp.Client.ExclusiveAddressUse = false;
        _udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        _udp.Client.Bind(new IPEndPoint(IPAddress.Any, discoveryPort));
        _receiveLoop = ReceiveLoopAsync(_shutdown.Token);
        _expiryLoop = ExpiryLoopAsync(_shutdown.Token);
    }

    public event EventHandler? SessionsChanged;

    public IReadOnlyList<DiscoveredSession> Snapshot() =>
        _sessions.Values
            .OrderBy(session => session.Announcement.ServerName, StringComparer.CurrentCultureIgnoreCase)
            .ThenBy(session => session.Address, StringComparer.Ordinal)
            .ToArray();

    private async Task ReceiveLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var received = await _udp.ReceiveAsync(cancellationToken).ConfigureAwait(false);
                var announcement = DiscoveryCodec.Decode(received.Buffer);
                _sessions[announcement.SessionId] = new DiscoveredSession(
                    announcement,
                    received.RemoteEndPoint.Address.ToString(),
                    DateTimeOffset.UtcNow);
                SessionsChanged?.Invoke(this, EventArgs.Empty);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (SocketException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (ProtocolException)
            {
                // Foreign or malformed datagrams are ignored.
            }
        }
    }

    private async Task ExpiryLoopAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
        {
            var cutoff = DateTimeOffset.UtcNow - TimeSpan.FromSeconds(4);
            var changed = false;
            foreach (var item in _sessions)
            {
                if (item.Value.LastSeenUtc < cutoff)
                {
                    changed |= _sessions.TryRemove(item.Key, out _);
                }
            }

            if (changed)
            {
                SessionsChanged?.Invoke(this, EventArgs.Empty);
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        _shutdown.Cancel();
        _udp.Dispose();
        foreach (var task in new[] { _receiveLoop, _expiryLoop })
        {
            try
            {
                await task.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        _shutdown.Dispose();
    }
}
