using System.ComponentModel;
using System.Diagnostics;

namespace AIMP.Launcher;

internal static class AiProcessDiscovery
{
    internal static AiProcessTarget Find()
    {
        var processes = Process.GetProcessesByName("AI");
        try
        {
            var candidates = processes.Select(TryRead).OfType<AiProcessTarget>().ToArray();
            // Split-screen: this launcher's own game first; never another launcher's.
            var own = candidates.Where(value => value.ProcessId == LauncherInstances.GameProcessId).ToArray();
            if (own.Length > 0) return SelectCandidate(own);
            var claimed = LauncherInstances.GamesClaimedByOthers();
            return SelectCandidate(candidates.Where(value => !claimed.Contains(value.ProcessId)).ToArray());
        }
        finally
        {
            foreach (var process in processes) process.Dispose();
        }
    }

    internal static AiProcessTarget SelectCandidate(
        IReadOnlyList<AiProcessTarget> candidates)
    {
        ArgumentNullException.ThrowIfNull(candidates);
        // Prefer an interactive game window, then the newest process.
        return candidates
            .Where(IsValid)
            .OrderByDescending(value => value.HasMainWindow)
            .ThenByDescending(value => value.StartTimeUtcTicks)
            .FirstOrDefault()
            ?? throw new InvalidOperationException("AI.exe is not running");
    }

    private static AiProcessTarget? TryRead(Process process)
    {
        try
        {
            process.Refresh();
            return process.HasExited ? null : Read(process);
        }
        catch (Exception exception) when (IsInspectionFailure(exception))
        {
            return null;
        }
    }

    private static AiProcessTarget Read(Process process)
    {
        var module = process.MainModule
            ?? throw new InvalidOperationException("AI.exe main module is unavailable.");
        return new AiProcessTarget(
            process.Id,
            Path.GetFullPath(module.FileName),
            module.BaseAddress,
            process.StartTime.ToUniversalTime().Ticks,
            process.MainWindowHandle != 0);
    }

    private static bool IsValid(AiProcessTarget value) =>
        value.ProcessId > 0
        && value.StartTimeUtcTicks > 0
        && string.Equals(Path.GetFileName(value.ExecutablePath), "AI.exe",
            StringComparison.OrdinalIgnoreCase);

    private static bool IsInspectionFailure(Exception exception) =>
        exception is Win32Exception or InvalidOperationException
            or NotSupportedException or SystemException;
}

internal static class NativeModuleInspector
{
    internal static bool IsLoaded(int processId, string dllPath)
    {
        using var process = Process.GetProcessById(processId);
        return process.Modules.Cast<ProcessModule>()
            .Any(module => IsMatch(module.ModuleName, module.FileName, dllPath));
    }

    internal static bool IsMatch(string moduleName, string modulePath, string dllPath)
    {
        // Matching by name closes the race where another launcher loaded the same DLL path.
        var expectedName = Path.GetFileName(dllPath);
        return string.Equals(moduleName, expectedName, StringComparison.OrdinalIgnoreCase)
            || PathsEqual(modulePath, dllPath);
    }

    private static bool PathsEqual(string left, string right)
    {
        if (string.IsNullOrWhiteSpace(left) || string.IsNullOrWhiteSpace(right)) return false;
        return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right),
            StringComparison.OrdinalIgnoreCase);
    }
}

internal static class NativeDllResolver
{
    private const string DllName = "AIMP.NetZ.dll";

    internal static string Resolve(string launcherDirectory)
    {
        var root = FindRepositoryRoot(launcherDirectory);
        var candidates = BuildCandidates(launcherDirectory, root);
        var path = candidates.FirstOrDefault(File.Exists);
        if (path is null) throw new FileNotFoundException(
            $"{DllName} was not found in the launcher or native build directories.");

        // The DLL delivered alongside this API belongs to the same build and
        // protocol. A repository build may be older, newer or another branch;
        // a different hash does not make it compatible. Use repository builds
        // only when no launcher-local DLL exists. HELLO remains strict.
        return path;
    }

    internal static IReadOnlyList<string> BuildCandidates(
        string launcherDirectory,
        string repositoryRoot)
    {
        var values = new List<string>
        {
            Path.Combine(launcherDirectory, DllName),
            Path.Combine(launcherDirectory, "Native", DllName)
        };
        // Launcher-local files always win. Repository Release builds precede
        // Debug builds only as a fallback for an unpackaged development API.
        // Within each configuration, prefer the most recently written DLL:
        // an older alphabetically first build must not shadow a fresh build.
        AddBuildCandidates(values, repositoryRoot, "Release");
        AddBuildCandidates(values, repositoryRoot, "Debug");
        return values.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    internal static string FindRepositoryRoot(string startDirectory)
    {
        for (var current = new DirectoryInfo(startDirectory); current is not null;
             current = current.Parent)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "AIMP.Native"))
                && Directory.Exists(Path.Combine(current.FullName, "AIMP.Launcher")))
            {
                return current.FullName;
            }
        }

        return string.Empty;
    }

    private static void AddBuildCandidates(
        ICollection<string> values,
        string repositoryRoot,
        string configuration)
    {
        var paths = EnumerateNativeBuilds(repositoryRoot).SelectMany(directory => new[]
        {
            Path.Combine(directory, "bin", configuration, DllName),
            Path.Combine(directory, configuration, DllName)
        });
        foreach (var path in paths.OrderByDescending(File.GetLastWriteTimeUtc)
                     .ThenBy(value => value, StringComparer.OrdinalIgnoreCase))
            values.Add(path);
    }

    private static IEnumerable<string> EnumerateNativeBuilds(string repositoryRoot)
    {
        if (string.IsNullOrWhiteSpace(repositoryRoot)) return Array.Empty<string>();
        var root = Path.Combine(repositoryRoot, "AIMP.Native", "out", "build");
        if (!Directory.Exists(root)) return Array.Empty<string>();
        return Directory.EnumerateDirectories(root).Order(StringComparer.OrdinalIgnoreCase);
    }
}

internal static class InjectorPathResolver
{
    internal static string Resolve(string launcherDirectory)
    {
        var local = Path.Combine(launcherDirectory, "Injector32", "AIMP.Injector32.exe");
        if (File.Exists(local)) return local;
        var root = NativeDllResolver.FindRepositoryRoot(launcherDirectory);
        var paths = BuildCandidates(root);
        return paths.FirstOrDefault(File.Exists)
            ?? throw new FileNotFoundException("AIMP.Injector32.exe was not found.");
    }

    private static IEnumerable<string> BuildCandidates(string repositoryRoot)
    {
        if (string.IsNullOrWhiteSpace(repositoryRoot)) yield break;
        var project = Path.Combine(repositoryRoot, "AIMP.Launcher", "src", "AIMP.Injector32");
        yield return Path.Combine(project, "bin", "x86", "Release", "net8.0", "AIMP.Injector32.exe");
        yield return Path.Combine(project, "bin", "x86", "Debug", "net8.0", "AIMP.Injector32.exe");
    }
}
