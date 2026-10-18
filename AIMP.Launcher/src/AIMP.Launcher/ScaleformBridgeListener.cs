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
    private TcpListener? _commandListener;
    private TcpListener? _eventListener;
    private TcpClient? _commandClient;
    private NetworkStream? _commandStream;
    private Task[] _acceptTasks = Array.Empty<Task>();
    private int _connectedClients;
    private int _disposed;

    internal ScaleformBridgeListener(LauncherDiagnostics diagnostics)
    {
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
    }

    internal event EventHandler<ScaleformBridgeState>? StateChanged;
    internal event EventHandler<ScaleformBridgeEvent>? MessageReceived;

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
            CreateListeners();
            StartAcceptLoops();
        }
        LogStarted();
        PublishState();
    }

    internal async Task<bool> SendAsync(
        string type,
        IReadOnlyDictionary<string, string>? values = null,
        CancellationToken cancellationToken = default)
    {
        var bytes = Encoding.UTF8.GetBytes(_protocol.Build(type, values) + "\n");
        await _sendGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try { return await WriteCommandAsync(type, bytes, cancellationToken).ConfigureAwait(false); }
        finally { _sendGate.Release(); }
    }

    internal async Task StopAsync()
    {
        var stopped = DetachRuntime();
        if (stopped is null) return;
        stopped.Value.Cancellation.Cancel();
        stopped.Value.Command.Stop();
        stopped.Value.Event.Stop();
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

    private void CreateListeners()
    {
        var command = new TcpListener(IPAddress.Loopback, CommandPort);
        var events = new TcpListener(IPAddress.Loopback, EventPort);
        command.Start();
        try { events.Start(); }
        catch { command.Stop(); throw; }
        _cancellation = new CancellationTokenSource();
        _commandListener = command;
        _eventListener = events;
    }

    private void StartAcceptLoops()
    {
        var token = _cancellation!.Token;
        _acceptTasks = new[]
        {
            AcceptLoopAsync(_commandListener!, "command", token),
            AcceptLoopAsync(_eventListener!, "event", token)
        };
    }

    private void LogStarted()
    {
        _diagnostics.Log("UI Bridge", $"Command bridge listening on 127.0.0.1:{CommandPort}.");
        _diagnostics.Log("UI Bridge", $"Event bridge listening on 127.0.0.1:{EventPort}.");
    }

    private async Task AcceptLoopAsync(
        TcpListener listener,
        string channel,
        CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            var client = await TryAcceptAsync(listener, channel, token).ConfigureAwait(false);
            if (client is not null) _ = HandleClientAsync(client, channel, token);
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
        if (await TryHandlePolicyAsync(stream, first, token).ConfigureAwait(false)) return;
        var command = RegisterCommandClient(channel, client, stream);
        AddConnection(channel);
        try { await ProcessMessagesAsync(stream, channel, first, token).ConfigureAwait(false); }
        finally { DisconnectClient(channel, client, command); }
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

    private async Task ProcessMessagesAsync(
        NetworkStream stream,
        string channel,
        string first,
        CancellationToken token)
    {
        PublishMessage(channel, first, stream);
        while (!token.IsCancellationRequested)
        {
            var message = await ReadMessageAsync(stream, token).ConfigureAwait(false);
            if (message is null) return;
            if (message.Length != 0) PublishMessage(channel, message, stream);
        }
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

    private void PublishMessage(string channel, string raw, NetworkStream stream)
    {
        var message = _protocol.Parse(raw);
        var details = _protocol.Describe(message);
        var suffix = details.Length == 0 ? string.Empty : $" | {details}";
        var type = message.Type.Length == 0 ? "UNKNOWN" : message.Type.ToUpperInvariant();
        _diagnostics.Log("UI Bridge", $"Event received: channel={channel}, type={type}{suffix}");
        MessageReceived?.Invoke(this, new ScaleformBridgeEvent(channel, raw, message,
            (replyType, values) => ReplyAsync(stream, replyType, values)));
    }

    // Replies on the exact connection that sent the message. Event-port
    // movies (e.g. the pause menu) never own the single command channel.
    private async Task<bool> ReplyAsync(
        NetworkStream stream,
        string type,
        IReadOnlyDictionary<string, string>? values)
    {
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

    private (CancellationTokenSource Cancellation, TcpListener Command,
        TcpListener Event, Task[] Tasks)? DetachRuntime()
    {
        lock (_gate)
        {
            var cancellation = _cancellation;
            var command = _commandListener;
            var events = _eventListener;
            if (cancellation is null || command is null || events is null) return null;
            var tasks = _acceptTasks;
            ClearRuntimeReferences();
            return (cancellation, command, events, tasks);
        }
    }

    private void ClearRuntimeReferences()
    {
        _cancellation = null;
        _commandListener = null;
        _eventListener = null;
        _acceptTasks = Array.Empty<Task>();
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
