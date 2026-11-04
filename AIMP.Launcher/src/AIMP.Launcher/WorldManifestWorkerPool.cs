using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace AIMP.Launcher;

// Compiles level manifests concurrently, one child process per level.
//
// A level compilation is single-threaded and takes about a minute and a half.
// Levels are independent, but they cannot share a process: CathodeLib keeps
// process-wide, unsynchronized tables (ShortGuid names, COMMANDS.BIN entity and
// parameter names) that concurrent parsers would corrupt. Each worker is this
// executable in its manifest CLI mode, so it runs the exact compiler and writes
// the same per-level cache file as an in-process compilation.
internal static class WorldManifestWorkerPool
{
    // Peak working set of one level compilation is about 1.7 GB.
    private const long WorkerMemoryBudget = 2560L * 1024 * 1024;
    private const int MaximumWorkers = 6;

    internal static string[] CompileAll(IReadOnlyList<string> levelDirectories,
        Action<int, int> progress, CancellationToken token)
    {
        var results = new string[levelDirectories.Count];
        var workers = WorkerCount(levelDirectories.Count);
        var host = workers > 1 ? ResolveHost() : null;
        if (host == null)
        {
            for (var i = 0; i < levelDirectories.Count; i++)
            {
                token.ThrowIfCancellationRequested();
                var index = i;
                results[i] = new WorldManifestCompiler().Compile(levelDirectories[i], progress:
                    new InlineProgress(value => progress(index, value))).BinaryPath;
            }
            return results;
        }

        // The first failure stops the remaining workers; report that failure.
        using var failed = CancellationTokenSource.CreateLinkedTokenSource(token);
        try
        {
            Parallel.For(0, levelDirectories.Count,
                new ParallelOptions { MaxDegreeOfParallelism = workers, CancellationToken = failed.Token },
                (index, _) =>
                {
                    try
                    {
                        results[index] = CompileInWorker(host.Value, levelDirectories[index],
                            value => progress(index, value), failed.Token);
                    }
                    catch (Exception error) when (error is not OperationCanceledException)
                    {
                        failed.Cancel();
                        throw;
                    }
                });
        }
        catch (AggregateException error)
        {
            token.ThrowIfCancellationRequested();
            var cause = error.Flatten().InnerExceptions.FirstOrDefault(inner => inner is not OperationCanceledException)
                ?? error.InnerExceptions[0];
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(cause).Throw();
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        {
            throw new InvalidDataException("A manifest compiler stopped the preparation.");
        }
        return results;
    }

    internal static int WorkerCount(int levels)
    {
        var byCores = Math.Max(1, Environment.ProcessorCount / 2);
        var byMemory = (int)Math.Max(1, AvailablePhysicalMemory() / WorkerMemoryBudget);
        return Math.Clamp(Math.Min(Math.Min(byCores, byMemory), levels), 1, MaximumWorkers);
    }

    private static string CompileInWorker((string FileName, string Prefix) host, string level,
        Action<int> progress, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        var start = new ProcessStartInfo(host.FileName)
        {
            Arguments = $"{host.Prefix}--compile-world-manifest {Quote(level)} {WorldManifestCli.WorkerSwitch}",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        using var process = new Process { StartInfo = start };
        string? result = null;
        var errors = new StringBuilder();
        process.OutputDataReceived += (_, line) =>
        {
            if (line.Data is not { } data) return;
            if (data.StartsWith(WorldManifestCli.WorkerProgressPrefix, StringComparison.Ordinal) &&
                int.TryParse(data.AsSpan(WorldManifestCli.WorkerProgressPrefix.Length), out var value))
                progress(value);
            else if (data.StartsWith(WorldManifestCli.WorkerResultPrefix, StringComparison.Ordinal))
                result = data[WorldManifestCli.WorkerResultPrefix.Length..];
        };
        process.ErrorDataReceived += (_, line) =>
        {
            if (line.Data is { } data) lock (errors) errors.AppendLine(data);
        };
        if (!process.Start()) throw new InvalidOperationException("The manifest compiler could not be started.");
        try { process.PriorityClass = ProcessPriorityClass.BelowNormal; }
        catch (InvalidOperationException) { } // Already exited.
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        using (token.Register(() => Kill(process)))
            process.WaitForExit();
        token.ThrowIfCancellationRequested();
        if (process.ExitCode != 0 || string.IsNullOrWhiteSpace(result))
        {
            string message;
            lock (errors) message = errors.ToString().Trim();
            throw new InvalidDataException(string.IsNullOrEmpty(message)
                ? $"The manifest compiler stopped unexpectedly ({process.ExitCode}) for '{level}'."
                : message);
        }
        return result;
    }

    // The API normally runs as AIMP.Launcher.exe; tests and development hosts
    // may run it through dotnet. Without a usable host, compile in-process.
    private static (string FileName, string Prefix)? ResolveHost()
    {
        var path = Environment.ProcessPath;
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
        if (string.Equals(Path.GetFileNameWithoutExtension(path), "AIMP.Launcher", StringComparison.OrdinalIgnoreCase))
            return (path, string.Empty);
        var assembly = typeof(WorldManifestWorkerPool).Assembly.Location;
        if (string.Equals(Path.GetFileNameWithoutExtension(path), "dotnet", StringComparison.OrdinalIgnoreCase) &&
            string.Equals(Path.GetFileName(assembly), "AIMP.Launcher.dll", StringComparison.OrdinalIgnoreCase))
            return (path, Quote(assembly) + " ");
        return null;
    }

    private static void Kill(Process process)
    {
        try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
    }

    private static string Quote(string value) => "\"" + value.TrimEnd('\\') + "\"";

    private static long AvailablePhysicalMemory()
    {
        var status = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        return GlobalMemoryStatusEx(ref status) ? (long)Math.Min(status.AvailablePhysical, long.MaxValue) : WorkerMemoryBudget;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatusEx
    {
        public uint Length;
        public uint MemoryLoad;
        public ulong TotalPhysical;
        public ulong AvailablePhysical;
        public ulong TotalPageFile;
        public ulong AvailablePageFile;
        public ulong TotalVirtual;
        public ulong AvailableVirtual;
        public ulong AvailableExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx buffer);

    private sealed class InlineProgress(Action<int> report) : IProgress<WorldManifestCompilerProgress>
    {
        public void Report(WorldManifestCompilerProgress value) => report(value.Percentage);
    }
}
