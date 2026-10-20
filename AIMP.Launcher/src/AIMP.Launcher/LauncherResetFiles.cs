namespace AIMP.Launcher;

internal static class LauncherResetFiles
{
    internal static string UserDataDirectory()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(local) || !Path.IsPathFullyQualified(local))
            throw new IOException("The launcher data directory is unavailable.");
        return Path.Combine(local, "AIMP");
    }

    internal static int DeleteGeneratedData(string root, string settingsPath, string gameDirectory)
    {
        root = Path.GetFullPath(root);
        EnsureNotLink(root);
        var cache = SafePath(root, "WorldManifestCache");
        var preparation = SafePath(root, "Preparation");
        var settings = RequireInside(root, settingsPath);
        var cacheFiles = CollectFiles(cache);
        var receiptFiles = CollectFiles(preparation);
        var gameManifests = CollectGameManifests(gameDirectory);
        EnsureNotLink(settings);
        foreach (var path in cacheFiles.Concat(receiptFiles).Concat(gameManifests))
        {
            EnsureNotLink(path);
            File.Delete(path);
        }
        RemoveEmptyTree(cache);
        RemoveEmptyTree(preparation);
        File.Delete(settings);
        return cacheFiles.Concat(gameManifests).Count(p => p.EndsWith(".aimpwm", StringComparison.OrdinalIgnoreCase));
    }

    private static string[] CollectGameManifests(string gameDirectory)
    {
        if (string.IsNullOrWhiteSpace(gameDirectory)) return [];
        if (!Path.IsPathFullyQualified(gameDirectory))
            throw new IOException("The configured game directory is not an absolute path.");
        var production = Path.Combine(Path.GetFullPath(gameDirectory), "DATA", "ENV", "PRODUCTION");
        return CollectFiles(production).Where(p =>
            p.EndsWith(".aimpwm", StringComparison.OrdinalIgnoreCase)
            || (Path.GetFileName(p).StartsWith("WORLD_MANIFEST_", StringComparison.OrdinalIgnoreCase)
                && p.EndsWith(".json", StringComparison.OrdinalIgnoreCase))).ToArray();
    }

    // Enumerate and validate the complete deletion plan before removing anything.
    private static string[] CollectFiles(string root)
    {
        EnsureNotLink(root);
        if (!Directory.Exists(root)) return [];
        var files = new List<string>();
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.TryPop(out var directory))
            foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
            {
                var path = RequireInside(root, entry);
                EnsureNotLink(path);
                if (Directory.Exists(path)) pending.Push(path);
                else files.Add(path);
            }
        return files.ToArray();
    }

    private static void RemoveEmptyTree(string root)
    {
        EnsureNotLink(root);
        if (!Directory.Exists(root)) return;
        foreach (var child in Directory.EnumerateDirectories(root))
            RemoveEmptyTree(RequireInside(root, child));
        Directory.Delete(root, recursive: false);
    }

    private static string SafePath(string root, string relative) => RequireInside(root, Path.Combine(root, relative));

    private static string RequireInside(string root, string path)
    {
        var prefix = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        var fullPath = Path.GetFullPath(path);
        if (!fullPath.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Launcher reset refused a path outside its data directory.");
        return fullPath;
    }

    private static void EnsureNotLink(string path)
    {
        for (FileInfo? current = new FileInfo(Path.GetFullPath(path)); current != null;
             current = current.Directory is { } parent ? new FileInfo(parent.FullName) : null)
            if (Path.Exists(current.FullName) && (File.GetAttributes(current.FullName) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Launcher reset does not follow filesystem links.");
    }
}
