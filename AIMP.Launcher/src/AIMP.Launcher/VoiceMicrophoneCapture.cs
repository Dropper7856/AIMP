using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace AIMP.Launcher;

/// <summary>Raw 16 kHz mono PCM16 capture. Gating, gain and transport belong to the caller.</summary>
internal sealed class VoiceMicrophoneCapture : IDisposable
{
    internal const int SamplesPerFrame = 320;
    private readonly object _gate = new();
    private Session? _session;
    private bool _disposed;
    private string _lastError = string.Empty;
    public event EventHandler<short[]>? FrameCaptured;
    public event EventHandler<string>? ErrorOccurred;
    public string LastError => Volatile.Read(ref _lastError);
    private void PublishFrame(short[] frame) => FrameCaptured?.Invoke(this, frame);

    public void Start(string microphoneId)
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_session is { Running: true } current && current.DeviceId == microphoneId) return;
            StopCore();
            Volatile.Write(ref _lastError, string.Empty);
            StartSessionCore(microphoneId);
        }
    }

    private void StartSessionCore(string microphoneId)
    {
        try
        {
            var session = new Session(this, microphoneId);
            _session = session;
            session.Start();
        }
        catch (Exception error)
        {
            StopCore();
            ReportError(error.Message);
            throw;
        }
    }

    public void Stop() { lock (_gate) StopCore(); }
    private void StopCore()
    {
        var session = _session;
        _session = null;
        // Native cleanup is synchronous; a frame handler already in progress may finish.
        session?.Stop();
    }
    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            StopCore();
        }
    }
    private void ReportError(string error)
    {
        Volatile.Write(ref _lastError, error);
        ThreadPool.QueueUserWorkItem(_ =>
        {
            try { ErrorOccurred?.Invoke(this, error); } catch { /* Error observers cannot stop cleanup. */ }
        });
    }

    private sealed class Session
    {
        private readonly VoiceMicrophoneCapture _owner;
        private readonly ConcurrentQueue<(nint Header, short[] Samples)> _returned = new();
        private readonly AutoResetEvent _ready = new(false);
        private readonly List<nint> _headers = new();
        private readonly WinMm.Callback _callback;
        private readonly object _nativeGate = new();
        private readonly Thread _worker;
        private static readonly ConcurrentBag<Session> RetainedSessions = new();
        private bool _cleaned;
        private bool _retained;
        private nint _handle;
        private short[] _frame = new short[SamplesPerFrame];
        private int _filled;
        private volatile bool _running;
        private long _lastData;
        private string? _callbackError;
        internal string DeviceId { get; }
        internal bool Running => _running;
        private static readonly uint HeaderSize = (uint)Marshal.SizeOf<WinMm.Header>();

        internal Session(VoiceMicrophoneCapture owner, string deviceId)
        {
            _owner = owner;
            DeviceId = deviceId;
            _callback = OnData;
            _worker = new Thread(Run) { IsBackground = true, Name = "Voice microphone" };
        }
        internal void Start()
        {
            try
            {
                Open();
                for (var i = 0; i < 4; i++) QueueNewBuffer();
                Volatile.Write(ref _lastData, Environment.TickCount64);
                _running = true;
                WinMm.Check(WinMm.waveInStart(_handle), "Start microphone");
                _worker.Start();
            }
            catch { _running = false; Cleanup(); if (!_retained) _ready.Dispose(); throw; }
        }
        private void Open()
        {
            var device = VoiceMicrophoneDevices.Resolve(DeviceId);
            var format = new WinMm.Format { Tag = 1, Channels = 1, SamplesPerSecond = 16000,
                BytesPerSecond = 32000, BlockAlign = 2, BitsPerSample = 16 };
            WinMm.Check(WinMm.waveInOpen(out _handle, device, ref format, _callback, 0,
                0x30000u | (device == uint.MaxValue ? 0x10u : 0)), "Open microphone");
        }
        private void QueueNewBuffer()
        {
            var data = Marshal.AllocHGlobal(SamplesPerFrame * 2);
            var pointer = Marshal.AllocHGlobal((int)HeaderSize);
            Marshal.StructureToPtr(new WinMm.Header { Data = data, BufferLength = SamplesPerFrame * 2 }, pointer, false);
            _headers.Add(pointer);
            WinMm.Check(WinMm.waveInPrepareHeader(_handle, pointer, HeaderSize), "Prepare microphone buffer");
            WinMm.Check(WinMm.waveInAddBuffer(_handle, pointer, HeaderSize), "Queue microphone buffer");
        }
        internal void Stop()
        {
            lock (_nativeGate)
            {
                if (_cleaned) return;
                _running = false;
                _ready.Set();
                Cleanup();
            }
        }
        private void OnData(nint handle, uint message, nuint instance, nint pointer, nuint reserved)
        {
            if (message != 0x3C0 || !_running) return;
            try
            {
                Volatile.Write(ref _lastData, Environment.TickCount64);
                // At most four outstanding headers; each is requeued only by the worker.
                _returned.Enqueue((pointer, CopySamples(pointer)));
                _ready.Set();
            }
            catch (Exception error)
            {
                _running = false;
                _callbackError = error.Message;
                _ready.Set();
            }
        }
        private static short[] CopySamples(nint pointer)
        {
            var header = Marshal.PtrToStructure<WinMm.Header>(pointer);
            var count = (int)Math.Min(header.BytesRecorded / 2, SamplesPerFrame);
            var samples = new short[count];
            Marshal.Copy(header.Data, samples, 0, count); // Copy before returning to WinMM.
            return samples;
        }
        private void Run()
        {
            try
            {
                while (_running)
                {
                    _ready.WaitOne(100);
                    if (_running && Environment.TickCount64 - Volatile.Read(ref _lastData) > 3000)
                        throw new InvalidOperationException("The microphone stopped delivering audio. Check that it is connected.");
                    DrainReturned();
                }
                if (_callbackError is not null) _owner.ReportError(_callbackError);
            }
            catch (Exception error) { _owner.ReportError(error.Message); }
            finally { _running = false; Cleanup(); if (!_retained) _ready.Dispose(); }
        }
        private void DrainReturned()
        {
            while (_running && _returned.TryDequeue(out var item))
            {
                lock (_nativeGate)
                    if (_running) WinMm.Check(WinMm.waveInAddBuffer(_handle, item.Header, HeaderSize), "Requeue microphone buffer");
                Accumulate(item.Samples);
            }
        }
        private void Accumulate(short[] samples)
        {
            foreach (var sample in samples)
            {
                _frame[_filled++] = sample;
                if (_filled != SamplesPerFrame) continue;
                if (!_running) return;
                try { _owner.PublishFrame(_frame); }
                catch (Exception error) { _owner.ReportError($"Voice frame handler: {error.Message}"); }
                _frame = new short[SamplesPerFrame];
                _filled = 0;
            }
        }
        private void Cleanup()
        {
            lock (_nativeGate)
            {
                if (_cleaned) return;
                _cleaned = true;
                // Never release memory still owned by a driver after an unexpected reset failure.
                if (_handle != 0 && !ReleaseNative()) return;
                _headers.Clear();
                // Keep the callback rooted through waveInClose. Failed closes retain the session.
                GC.KeepAlive(_callback);
            }
        }
        private bool ReleaseNative()
        {
            if (WinMm.waveInReset(_handle) != 0) { Retain("Microphone reset failed; native buffers retained for safety."); return false; }
            foreach (var pointer in _headers)
            {
                var header = Marshal.PtrToStructure<WinMm.Header>(pointer);
                if ((header.Flags & 2) != 0 && WinMm.waveInUnprepareHeader(_handle, pointer, HeaderSize) != 0)
                { Retain("Microphone buffer release failed; native buffer retained for safety."); continue; }
                Marshal.FreeHGlobal(header.Data);
                Marshal.FreeHGlobal(pointer);
            }
            var result = WinMm.waveInClose(_handle);
            if (result != 0) Retain($"Close microphone failed (WinMM {result}).");
            _handle = 0;
            return true;
        }
        private void Retain(string error)
        {
            if (!_retained) RetainedSessions.Add(this);
            _retained = true;
            _owner.ReportError(error);
        }
    }
}
