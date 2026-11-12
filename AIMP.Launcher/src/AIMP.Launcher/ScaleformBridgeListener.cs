using System.Net;
using System.Net.Sockets;
using System.Text;

namespace AIMP.Launcher;

internal sealed record ScaleformBridgeState(bool Running, int ConnectedClients)
{
    internal bool Connected => ConnectedClients > 0;
}

internal sealed class ScaleformBridgeListener : IDisposable
{
    internal const int CommandPort = 12345;
    internal const int EventPort = 12346;
    private const int MaximumMessageBytes = 64 * 1024;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly ScaleformBridgeProtocol _protocol = new();
    private readonly object _gate = new();
    private readonly object _commandGate = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private CancellationTokenSource? _cancellation;
    private readonly List<TcpListener> _listeners = new();
    private TcpClient? _commandClient;
    private NetworkStream? _commandStream;
    private readonly List<Task> _loops = new();
    private int _connectedClients;
    private int _disposed;

    internal ScaleformBridgeListener(LauncherDiagnostics diagnostics)
    {
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
    }

    internal event EventHandler<ScaleformBridgeState>? StateChanged;
    internal event EventHandler<ScaleformBridgeEvent>? MessageReceived;
    internal event EventHandler<ScaleformBridgeConnectionClosed>? ConnectionClosed;

    // HELLO starts automatic injection. Do not feed incoming Socket events to
    // an unprotected GFx heap while that injection is still in progress.
    internal Func<bool>? NativeUiReady { get; set; }

    internal ScaleformBridgeState State
    {
        get { lock (_gate) return CreateState(); }
    }

    internal void Start()
    {
        lock (_gate)
        {
            if (_cancellation is not null) return;
            ThrowIfDisposed();
            var cancellation = new CancellationTokenSource();
            var slot = LauncherInstances.Slot;
            var command = new TcpListener(IPAddress.Loopback, LauncherInstances.PrivateCommandPort(slot));
            var events = new TcpListener(IPAddress.Loopback, LauncherInstances.PrivateEventPort(slot));
            try { command.Start(); events.Start(); }
            catch { command.Stop(); events.Stop(); cancellation.Dispose(); throw; }
            _cancellation = cancellation;
            AddListener(command, "command", routed: false, cancellation.Token);
            AddListener(events, "event", routed: false, cancellation.Token);
            _loops.Add(AcquirePublicPortsAsync(cancellation.Token));
        }
        _diagnostics.Log("UI Bridge", $"Launcher instance {LauncherInstances.Slot}: private bridge on 127.0.0.1:" +
            $"{LauncherInstances.PrivateCommandPort(LauncherInstances.Slot)}/{LauncherInstances.PrivateEventPort(LauncherInstances.Slot)}.");
        PublishState();
    }

    internal async Task<bool> SendAsync(
        string type,
        IReadOnlyDictionary<string, string>? values = null,
        CancellationToken cancellationToken = default)
    {
        if (NativeUiReady?.Invoke() != true) return false;
        var bytes = Encoding.UTF8.GetBytes(_protocol.Build(type, values) + "\n");
        await _sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try { return await WriteCommandAsync(type, bytes, cancellationToken).ConfigureAwait(false); }
        finally { _sendGate.Release(); }
    }

    internal async Task StopAsync()
    {
        var stopped = DetachRuntime();
        if (stopped is null) return;
        foreach (var listener in stopped.Value.Listeners) listener.Stop();
        CloseCommandClient();
        await AwaitLoopsAsync(stopped.Value.Tasks).ConfigureAwait(false);
        stopped.Value.Cancellation.Dispose();
        ResetConnections();
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        StopAsync().GetAwaiter().GetResult();
        _sendGate.Dispose();
    }

    // Caller holds _gate.
    private void AddListener(TcpListener listener, string channel, bool routed, CancellationToken token)
    {
        _listeners.Add(listener);
        _loops.Add(AcceptLoopAsync(listener, channel, routed, token));
    }

    // The game's movies always connect to 12345/12346. Only one launcher can
    // hold them; the others retry so one of them takes over when it closes.
    private async Task AcquirePublicPortsAsync(CancellationToken token)
    {
        var waiting = false;
        while (!token.IsCancellationRequested)
        {
            if (TryListenPublic(token)) return;
            if (!waiting)
                _diagnostics.Log("UI Bridge", $"127.0.0.1:{CommandPort}/{EventPort} are in use (another AI-MP Launcher?). " +
                    "Its launcher forwards this launcher's game; retrying every 2 s.");
            waiting = true;
            try { await Task.Delay(TimeSpan.FromSeconds(2), token).ConfigureAwait(false); }
            catch (OperationCanceledException) { return; }
        }
    }

    private bool TryListenPublic(CancellationToken token)
    {
        var command = new TcpListener(IPAddress.Loopback, CommandPort);
        var events = new TcpListener(IPAddress.Loopback, EventPort);
        try { command.Start(); events.Start(); }
        catch (SocketException) { command.Stop(); events.Stop(); return false; }
        lock (_gate)
        {
            if (token.IsCancellationRequested) { command.Stop(); events.Stop(); return true; }
            AddListener(command, "command", routed: true, token);
            AddListener(events, "event", routed: true, token);
        }
        _diagnostics.Log("UI Bridge", $"Command bridge listening on 127.0.0.1:{CommandPort}.");
        _diagnostics.Log("UI Bridge", $"Event bridge listening on 127.0.0.1:{EventPort}.");
        return true;
    }

    private async Task AcceptLoopAsync(
        TcpListener listener,
        string channel,
        bool routed,
        CancellationToken token)
    {
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        while (!token.IsCancellationRequested)
        {
            var client = await TryAcceptAsync(listener, channel, token).ConfigureAwait(false);
            if (client is null) continue;
            _ = routed ? RouteClientAsync(client, channel, port, token) : HandleClientAsync(client, channel, token);
        }
    }

    // A connection on the public ports from another launcher's game goes to
    // that launcher's private port; everything else is handled here.
    private async Task RouteClientAsync(TcpClient client, string channel, int port, CancellationToken token)
    {
        int? slot = null;
        try { slot = LauncherInstances.ForwardSlotFor(client.Client.RemoteEndPoint as IPEndPoint, port); }
        catch (Exception exception) when (exception is not OperationCanceledException)
        { _diagnostics.Log("UI Bridge", $"{channel} owner lookup failed: {exception.Message}"); }
        if (slot is null) await HandleClientAsync(client, channel, token).ConfigureAwait(false);
        else await ForwardClientAsync(client, channel, slot.Value, token).ConfigureAwait(false);
    }

    private async Task ForwardClientAsync(TcpClient client, string channel, int slot, CancellationToken token)
    {
        using (client)
        using (var upstream = new TcpClient())
        {
            var port = channel == "command"
                ? LauncherInstances.PrivateCommandPort(slot) : LauncherInstances.PrivateEventPort(slot);
            try
            {
                await upstream.ConnectAsync(IPAddress.Loopback, port, token).ConfigureAwait(false);
                _diagnostics.Log("UI Bridge", $"Forwarding a {channel} connection to launcher instance {slot} (port {port}).");
                using var linked = CancellationTokenSource.CreateLinkedTokenSource(token);
                var up = client.GetStream().CopyToAsync(upstream.GetStream(), linked.Token);
                var down = upstream.GetStream().CopyToAsync(client.GetStream(), linked.Token);
                await Task.WhenAny(up, down).ConfigureAwait(false);
                linked.Cancel();
                await Task.WhenAll(up, down).ConfigureAwait(false);
            }
            catch (Exception exception) when (exception is IOException or SocketException
                or OperationCanceledException or ObjectDisposedException) { }
        }
    }

    private async Task<TcpClient?> TryAcceptAsync(
        TcpListener listener,
        string channel,
        CancellationToken token)
    {
        try { return await listener.AcceptTcpClientAsync(token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { return null; }
        catch (SocketException) when (token.IsCancellationRequested) { return null; }
        catch (Exception exception) { LogAcceptFailure(channel, exception); return null; }
    }

    private void LogAcceptFailure(string channel, Exception exception)
    {
        _diagnostics.Log("UI Bridge", $"{channel} accept failed: {exception.Message}");
        Thread.Sleep(100);
    }

    private async Task HandleClientAsync(
        TcpClient client,
        string channel,
        CancellationToken token)
    {
        using (client)
        {
            try { await HandleConnectedClientAsync(client, channel, token).ConfigureAwait(false); }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { }
            catch (Exception exception) { LogClientFailure(channel, exception); }
        }
    }

    private async Task HandleConnectedClientAsync(
        TcpClient client,
        string channel,
        CancellationToken token)
    {
        var stream = client.GetStream();
        var first = await ReadMessageAsync(stream, token).ConfigureAwait(false);
        if (first is null) return;
        if (await TryHandlePolicyAsync(stream, first, token).ConfigureAwait(false) || RejectForeignConnection(channel, first)) return;
        var connectionId = Guid.NewGuid();
        var command = RegisterCommandClient(channel, client, stream);
        AddConnection(channel);
        var peerClosed = false;
        try { peerClosed = await ProcessMessagesAsync(stream, channel, first, connectionId, token).ConfigureAwait(false); }
        catch (IOException exception) when (!token.IsCancellationRequested
            && exception.InnerException is SocketException { SocketErrorCode: SocketError.ConnectionReset })
        {
            // GFx may reset its TCP connection when Socket.close is called.
            // This is peer termination, unlike a local replacement/disposal.
            peerClosed = true;
            LogClientFailure(channel, exception);
        }
        finally
        {
            lock (_commandGate)
                if (command && !ReferenceEquals(_commandClient, client)) peerClosed = false;
            DisconnectClient(channel, client, command);
            ConnectionClosed?.Invoke(this, new ScaleformBridgeConnectionClosed(channel, connectionId, peerClosed));
        }
    }

    private bool RegisterCommandClient(string channel, TcpClient client, NetworkStream stream)
    {
        if (!string.Equals(channel, "command", StringComparison.Ordinal)) return false;
        lock (_commandGate)
        {
            _commandClient?.Dispose();
            _commandClient = client;
            _commandStream = stream;
        }
        return true;
    }

    private void DisconnectClient(string channel, TcpClient client, bool command)
    {
        if (command) ClearCommandClient(client);
        RemoveConnection(channel);
    }

    private static async Task<bool> TryHandlePolicyAsync(
        NetworkStream stream,
        string message,
        CancellationToken token)
    {
        if (!string.Equals(message, "<policy-file-request/>", StringComparison.Ordinal))
            return false;
        await stream.WriteAsync(ScaleformBridgeProtocol.PolicyResponse(), token)
            .ConfigureAwait(false);
        return true;
    }

    // Web pages can reach loopback ports: a fetch or form POST to 127.0.0.1:12345
    // arrives as an HTTP request whose body lines would otherwise run as menu
    // commands (and replace the game's command connection). The game's movies
    // never send HTTP or binary data, so such connections are dropped unread.
    private bool RejectForeignConnection(string channel, string first)
    {
        if (!LooksForeign(first)) return false;
        _diagnostics.Log("UI Bridge", $"Rejected a {channel} connection that is not from the game (HTTP or binary data).");
        return true;
    }

    internal static bool LooksForeign(string first) =>
        first.Any(character => char.IsControl(character) && character != '\t')
        || HttpRequestLine.IsMatch(first);

    private static readonly System.Text.RegularExpressions.Regex HttpRequestLine = new(
        @"^[A-Za-z]{3,10} +\S+ +HTTP/\d",
        System.Text.RegularExpressions.RegexOptions.CultureInvariant | System.Text.RegularExpressions.RegexOptions.IgnoreCase);

    private async Task<bool> ProcessMessagesAsync(
        NetworkStream stream,
        string channel,
        string first,
        Guid connectionId,
        CancellationToken token)
    {
        PublishMessage(channel, first, stream, connectionId);
        while (!token.IsCancellationRequested)
        {
            var message = await ReadMessageAsync(stream, token).ConfigureAwait(false);
            if (message is null) return true;
            if (message.Length != 0) PublishMessage(channel, message, stream, connectionId);
        }
        return false;
    }

    private static async Task<string?> ReadMessageAsync(
        NetworkStream stream,
        CancellationToken token)
    {
        var bytes = new List<byte>(256);
        var buffer = new byte[1];
        while (bytes.Count <= MaximumMessageBytes)
        {
            var count = await stream.ReadAsync(buffer.AsMemory(0, 1), token).ConfigureAwait(false);
            if (count == 0) return bytes.Count == 0 ? null : Decode(bytes);
            if (buffer[0] is 0 or 10) return Decode(bytes);
            if (buffer[0] != 13) bytes.Add(buffer[0]);
        }
        throw new InvalidDataException("The UI bridge message exceeded 64 KiB.");
    }

    private void PublishMessage(string channel, string raw, NetworkStream stream, Guid connectionId)
    {
        var message = _protocol.Parse(raw);
        var details = _protocol.Describe(message);
        var suffix = details.Length == 0 ? string.Empty : $" | {details}";
        var type = message.Type.Length == 0 ? "UNKNOWN" : message.Type.ToUpperInvariant();
        _diagnostics.Log("UI Bridge", $"Event received: channel={channel}, type={type}{suffix}");
        MessageReceived?.Invoke(this, new ScaleformBridgeEvent(channel, raw, message,
            (replyType, values) => ReplyAsync(stream, replyType, values), connectionId));
    }

    // Replies on the exact connection that sent the message. Event-port
    // movies (e.g. the pause menu) never own the single command channel.
    private async Task<bool> ReplyAsync(
        NetworkStream stream,
        string type,
        IReadOnlyDictionary<string, string>? values)
    {
        if (NativeUiReady?.Invoke() != true) return false;
        var bytes = Encoding.UTF8.GetBytes(_protocol.Build(type, values) + "\n");
        await _sendGate.WaitAsync().ConfigureAwait(false);
        try { await stream.WriteAsync(bytes).ConfigureAwait(false); return true; }
        catch (Exception exception) when (exception is IOException or SocketException
            or ObjectDisposedException) { LogSendFailure(type, exception); return false; }
        finally { _sendGate.Release(); }
    }

    private async Task<bool> WriteCommandAsync(
        string type,
        byte[] bytes,
        CancellationToken token)
    {
        var stream = GetCommandStream();
        if (stream is null) return false;
        try { await stream.WriteAsync(bytes, token).ConfigureAwait(false); return true; }
        catch (Exception exception) when (exception is IOException or SocketException
            or ObjectDisposedException)
        { LogSendFailure(type, exception); CloseCommandClient(); return false; }
    }

    private NetworkStream? GetCommandStream()
    {
        lock (_commandGate) return _commandClient?.Connected == true ? _commandStream : null;
    }

    private void LogSendFailure(string type, Exception exception) =>
        _diagnostics.Log("UI Bridge", $"TCP send failed: type={type} | {exception.Message}");

    private void ClearCommandClient(TcpClient client)
    {
        lock (_commandGate)
        {
            if (!ReferenceEquals(_commandClient, client)) return;
            _commandClient = null;
            _commandStream = null;
        }
    }

    private void CloseCommandClient()
    {
        lock (_commandGate)
        {
            _commandClient?.Dispose();
            _commandClient = null;
            _commandStream = null;
        }
    }

    private void AddConnection(string channel)
    {
        Interlocked.Increment(ref _connectedClients);
        _diagnostics.Log("UI Bridge", $"{channel} client connected.");
        PublishState();
    }

    private void RemoveConnection(string channel)
    {
        if (Interlocked.Decrement(ref _connectedClients) < 0)
            Interlocked.Exchange(ref _connectedClients, 0);
        _diagnostics.Log("UI Bridge", $"{channel} client disconnected.");
        PublishState();
    }

    private void LogClientFailure(string channel, Exception exception) =>
        _diagnostics.Log("UI Bridge", $"{channel} client failed: {exception.Message}");

    // Cancels under _gate so the public-port retry cannot add listeners after detach.
    private (CancellationTokenSource Cancellation, TcpListener[] Listeners, Task[] Tasks)? DetachRuntime()
    {
        lock (_gate)
        {
            var cancellation = _cancellation;
            if (cancellation is null) return null;
            cancellation.Cancel();
            var listeners = _listeners.ToArray();
            var tasks = _loops.ToArray();
            ClearRuntimeReferences();
            return (cancellation, listeners, tasks);
        }
    }

    private void ClearRuntimeReferences()
    {
        _cancellation = null;
        _listeners.Clear();
        _loops.Clear();
    }

    private async Task AwaitLoopsAsync(IEnumerable<Task> tasks)
    {
        try { await Task.WhenAll(tasks).ConfigureAwait(false); }
        catch (OperationCanceledException) { }
        catch (Exception exception)
        { _diagnostics.Log("UI Bridge", $"Bridge shutdown warning: {exception.Message}"); }
    }

    private void ResetConnections()
    {
        Interlocked.Exchange(ref _connectedClients, 0);
        _diagnostics.Log("UI Bridge", "Scaleform bridges stopped.");
        PublishState();
    }

    private ScaleformBridgeState CreateState() => new(
        _cancellation is not null,
        Math.Max(0, Volatile.Read(ref _connectedClients)));

    private void PublishState() => StateChanged?.Invoke(this, State);

    private void ThrowIfDisposed()
    {
        if (Volatile.Read(ref _disposed) != 0)
            throw new ObjectDisposedException(nameof(ScaleformBridgeListener));
    }

    private static string Decode(IEnumerable<byte> bytes) =>
        Encoding.UTF8.GetString(bytes.ToArray());
}
