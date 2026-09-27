using System.ComponentModel;
using System.Diagnostics;
using System.Security.Cryptography;

namespace AIMP.Launcher;

internal static class AiProcessDiscovery
{
    internal static AiProcessTarget Find()
    {
        var processes = Process.GetProcessesByName("AI");
        try
        {
            var candidates = processes.Select(TryRead).OfType<AiProcessTarget>().ToArray();
            return SelectCandidate(candidates);
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

        // Development trees frequently retain an old DLL beside a previously
        // built Launcher. Keep packaged/local priority when the bytes match, but
        // if a repository native build exists and differs, prefer that build so
        // the Launcher cannot silently inject a stale copy. The strict HELLO
        // version check remains the final compatibility guard.
        if (!string.IsNullOrWhiteSpace(root)
            && IsLauncherLocalCandidate(path, launcherDirectory))
        {
            var repositoryBuild = candidates
                .Skip(2)
                .FirstOrDefault(File.Exists);
            if (repositoryBuild is not null && !FilesMatch(path, repositoryBuild))
                return repositoryBuild;
        }
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
        // Packaged files win when they match the repository build; native
        // Release builds precede Debug builds as the stale-copy fallback.
        AddBuildCandidates(values, repositoryRoot, "Release");
        AddBuildCandidates(values, repositoryRoot, "Debug");
        return values.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private static bool IsLauncherLocalCandidate(string path, string launcherDirectory)
    {
        var full = Path.GetFullPath(path);
        return string.Equals(full, Path.GetFullPath(Path.Combine(launcherDirectory, DllName)),
                   StringComparison.OrdinalIgnoreCase)
            || string.Equals(full, Path.GetFullPath(Path.Combine(launcherDirectory, "Native", DllName)),
                StringComparison.OrdinalIgnoreCase);
    }

    private static bool FilesMatch(string left, string right)
    {
        var leftInfo = new FileInfo(left);
        var rightInfo = new FileInfo(right);
        if (leftInfo.Length != rightInfo.Length) return false;
        using var leftStream = File.OpenRead(left);
        using var rightStream = File.OpenRead(right);
        return SHA256.HashData(leftStream).AsSpan().SequenceEqual(SHA256.HashData(rightStream));
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
        foreach (var directory in EnumerateNativeBuilds(repositoryRoot))
        {
            values.Add(Path.Combine(directory, "bin", configuration, DllName));
            values.Add(Path.Combine(directory, configuration, DllName));
        }
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
