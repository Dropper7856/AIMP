using System.Net;
using System.Net.Sockets;

namespace AIMP.Protocol;

public sealed class LanDiscoveryAnnouncer : IAsyncDisposable
{
    private readonly UdpClient _udp = new(AddressFamily.InterNetwork);
    private readonly Func<DiscoveryAnnouncement> _announcementProvider;
    private readonly IPEndPoint _destination;
    private readonly CancellationTokenSource _shutdown = new();
    private Task? _loop;

    public LanDiscoveryAnnouncer(
        Func<DiscoveryAnnouncement> announcementProvider,
        int discoveryPort = ProtocolConstants.DefaultDiscoveryPort)
    {
        ArgumentNullException.ThrowIfNull(announcementProvider);
        if (discoveryPort is < 1 or > 65535)
        {
            throw new ArgumentOutOfRangeException(nameof(discoveryPort));
        }

        _announcementProvider = announcementProvider;
        _destination = new IPEndPoint(IPAddress.Broadcast, discoveryPort);
        _udp.EnableBroadcast = true;
    }

    public void Start()
    {
        if (_loop is not null)
        {
            throw new InvalidOperationException("The discovery announcer is already running.");
        }

        _loop = RunAsync(_shutdown.Token);
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
        do
        {
            try
            {
                var datagram = DiscoveryCodec.Encode(_announcementProvider());
                await _udp.SendAsync(datagram, _destination, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (SocketException)
            {
                // A temporarily unavailable network interface must not stop the lobby.
            }
        }
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false));
    }

    public async ValueTask DisposeAsync()
    {
        _shutdown.Cancel();
        _udp.Dispose();
        if (_loop is not null)
        {
            try
            {
                await _loop.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        _shutdown.Dispose();
    }
}
