using System.Runtime.InteropServices;
using AIMP.Protocol;

namespace AIMP.Launcher;

internal static class WorldManifestCli
{
    private const string CompileSwitch = "--compile-world-manifest";
    internal const string WorkerSwitch = "--worker";
    internal const string WorkerProgressPrefix = "AIMP_PROGRESS ";
    internal const string WorkerResultPrefix = "AIMP_RESULT ";

    public static bool TryRun(string[] args, out int exitCode)
    {
        exitCode = 0;
        if (args.Length == 0 || !args[0].Equals(CompileSwitch, StringComparison.OrdinalIgnoreCase)) return false;

        // A preparation worker reports through redirected pipes, not a console.
        var worker = args.Contains(WorkerSwitch, StringComparer.OrdinalIgnoreCase);
        if (!worker) ToolConsole.AttachToParent();

        try
        {
            if (args.Length < 2)
            {
                throw new ArgumentException($"Usage: AIMP.Launcher.exe {CompileSwitch} <level-or-world-directory> [--output <file.aimpwm>] [--json <file.json>] [--no-cache]");
            }

            var sourcePath = args[1];
            string? outputPath = null;
            string? jsonPath = null;
            var useCache = true;

            for (var index = 2; index < args.Length; index++)
            {
                var current = args[index];
                if (current.Equals("--no-cache", StringComparison.OrdinalIgnoreCase))
                {
                    useCache = false;
                    continue;
                }

                if (current.Equals(WorkerSwitch, StringComparison.OrdinalIgnoreCase)) continue;

                if (current.Equals("--output", StringComparison.OrdinalIgnoreCase))
                {
                    outputPath = RequireValue(args, ref index, current);
                    continue;
                }

                if (current.Equals("--json", StringComparison.OrdinalIgnoreCase))
                {
                    jsonPath = RequireValue(args, ref index, current);
                    continue;
                }

                throw new ArgumentException($"Unknown manifest compiler option '{current}'.");
            }

            var progress = worker
                ? new WorkerProgress(value => Console.WriteLine(WorkerProgressPrefix + value.Percentage))
                : null;
            var result = new WorldManifestCompiler().Compile(sourcePath, outputPath, jsonPath, useCache, progress);
            if (worker) Console.WriteLine(WorkerResultPrefix + result.BinaryPath);
            else WriteSummary(result);
            return true;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"WorldManifestCompiler failed: {exception.Message}");
            exitCode = 2;
            return true;
        }
    }

    private static string RequireValue(string[] args, ref int index, string option)
    {
        index++;
        if (index >= args.Length || string.IsNullOrWhiteSpace(args[index]))
        {
            throw new ArgumentException($"Option '{option}' requires a value.");
        }
        return args[index];
    }

    private static void WriteSummary(WorldManifestCompilationResult result)
    {
        var manifest = result.Manifest;
        Console.WriteLine($"WorldManifest {(result.CacheHit ? "cache hit" : "compiled")}: {manifest.MapContentId}");
        Console.WriteLine($"  entities={manifest.Entities.Count} connections={manifest.Connections.Count} references={manifest.References.Count} paths={manifest.InstancePaths.Count} zones={manifest.Zones.Count}");
        Console.WriteLine($"  AIMP excluded composites={manifest.ExcludedAimpCompositeCount} entities={manifest.ExcludedAimpEntityCount}");
        Console.WriteLine($"  output={result.BinaryPath}");
        if (!string.IsNullOrWhiteSpace(result.JsonPath)) Console.WriteLine($"  json={result.JsonPath}");
        foreach (var warning in manifest.Warnings) Console.WriteLine($"  warning: {warning}");
    }

    private sealed class WorkerProgress(Action<WorldManifestCompilerProgress> report) : IProgress<WorldManifestCompilerProgress>
    {
        public void Report(WorldManifestCompilerProgress value) => report(value);
    }

    private static class ToolConsole
    {
        private const uint AttachParentProcess = 0xFFFFFFFFU;

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool AttachConsole(uint processId);

        public static void AttachToParent()
        {
            if (!OperatingSystem.IsWindows()) return;
            _ = AttachConsole(AttachParentProcess);

            try
            {
                var stdout = Console.OpenStandardOutput();
                if (stdout != Stream.Null) Console.SetOut(new StreamWriter(stdout) { AutoFlush = true });

                var stderr = Console.OpenStandardError();
                if (stderr != Stream.Null) Console.SetError(new StreamWriter(stderr) { AutoFlush = true });
            }
            catch (IOException)
            {
                // Tool output is diagnostic only; manifest generation must not depend on a console.
            }
        }
    }
}
