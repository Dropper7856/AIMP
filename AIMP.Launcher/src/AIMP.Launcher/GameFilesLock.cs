namespace AIMP.Launcher;

// Serializes writes to the game installation across launcher instances
// (split-screen runs two launchers on one installation). Hold it only from
// synchronous code: a Mutex must be released by the thread that owns it.
internal static class GameFilesLock
{
    private const string Name = @"Local\AIMP.Launcher.GameFiles";

    internal static IDisposable Acquire(CancellationToken token, Action? waiting = null)
    {
        var mutex = new Mutex(false, Name);
        try
        {
            var notified = false;
            while (!Wait(mutex, TimeSpan.FromMilliseconds(250)))
            {
                token.ThrowIfCancellationRequested();
                if (!notified) waiting?.Invoke();
                notified = true;
            }
            return new Releaser(mutex);
        }
        catch
        {
            mutex.Dispose();
            throw;
        }
    }

    // A launcher that crashed while holding the lock leaves it abandoned; its
    // file writes are atomic replacements, so the next owner simply continues.
    private static bool Wait(Mutex mutex, TimeSpan timeout)
    {
        try { return mutex.WaitOne(timeout); }
        catch (AbandonedMutexException) { return true; }
    }

    private sealed class Releaser(Mutex mutex) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            mutex.ReleaseMutex();
            mutex.Dispose();
        }
    }
}
