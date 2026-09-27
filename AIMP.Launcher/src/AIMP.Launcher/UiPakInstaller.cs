using System.Collections;
using System.Reflection;

namespace AIMP.Launcher;

internal sealed record UiBridgeAsset(string PakPath, string SourcePath);

internal static class UiBridgeAssetCatalog
{
    private static readonly string[] Names =
    {
        "NEWFRONTENDMENU.GFX",
        "PICKUPOVERLAY.GFX",
        "CHALLENGEMAPOVERLAY.GFX",
        "LOADINGSCREEN.GFX"
    };

    internal static IReadOnlyList<UiBridgeAsset> Resolve(string outputDirectory) =>
        Names.Select(name => new UiBridgeAsset(
            $"DATA/UI/{name}", Path.Combine(outputDirectory, "UIBridge", name))).ToArray();
}

internal sealed class UiPakInstaller
{
    private readonly LauncherDiagnostics _diagnostics;

    internal UiPakInstaller(LauncherDiagnostics diagnostics)
    {
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
    }

    internal void Install(GameInstallationPaths game)
    {
        var assets = UiBridgeAssetCatalog.Resolve(AppContext.BaseDirectory);
        ValidateAssets(assets);
        var backup = EnsureBackup(game.UiPakPath);
        try { InstallCore(game.UiPakPath, assets); }
        catch { RestoreAfterFailure(backup, game.UiPakPath); throw; }
    }

    private void InstallCore(string uiPakPath, IReadOnlyList<UiBridgeAsset> assets)
    {
        var pakType = LoadCathodeAssembly().GetType("CATHODE.PAK2")
            ?? throw new InvalidOperationException("CATHODE.PAK2 was not found in CathodeLib.");
        var pak = CreateAndLoadPak2(pakType, uiPakPath);
        foreach (var asset in assets) ApplyAsset(pak, asset);
        SavePak2(pak, uiPakPath);
        _diagnostics.Log("UI Install", $"UI.PAK updated with {assets.Count} bridge GFX files.");
    }

    private static void ValidateAssets(IEnumerable<UiBridgeAsset> assets)
    {
        var missing = assets.Where(asset => !File.Exists(asset.SourcePath)).ToArray();
        if (missing.Length == 0) return;
        throw new FileNotFoundException("A required UI bridge GFX file is missing.",
            missing[0].SourcePath);
    }

    private string EnsureBackup(string uiPakPath)
    {
        var backup = BackupPath();
        if (File.Exists(backup)) { LogExistingBackup(backup); return backup; }
        Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
        File.Copy(uiPakPath, backup, false);
        _diagnostics.Log("UI Install", $"Original UI.PAK backup created: {backup}");
        return backup;
    }

    private void LogExistingBackup(string backup) =>
        _diagnostics.Log("UI Install", $"Using existing UI.PAK backup: {backup}");

    private void RestoreAfterFailure(string backup, string uiPakPath)
    {
        File.Copy(backup, uiPakPath, true);
        _diagnostics.Log("UI Install", "UI injection failed; UI.PAK was restored from backup.");
    }

    private void ApplyAsset(object pak, UiBridgeAsset asset)
    {
        var entries = GetMemberValue(pak, "Entries")
            ?? throw new InvalidOperationException("CATHODE.PAK2 does not expose Entries.");
        var target = FindEntry(entries, asset.PakPath)
            ?? throw new InvalidOperationException($"UI.PAK entry was not found: {asset.PakPath}");
        ReplaceEntryData(target, File.ReadAllBytes(asset.SourcePath));
        _diagnostics.Log("UI Install", $"Prepared UI.PAK entry: {asset.PakPath}");
    }

    private static Assembly LoadCathodeAssembly()
    {
        var loaded = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(assembly =>
            string.Equals(assembly.GetName().Name, "CathodeLib", StringComparison.OrdinalIgnoreCase));
        return loaded ?? Assembly.Load("CathodeLib");
    }

    private static object CreateAndLoadPak2(Type pakType, string uiPakPath)
    {
        var constructor = pakType.GetConstructor(new[] { typeof(string) });
        if (constructor is not null) return constructor.Invoke(new object[] { uiPakPath });
        var pak = Activator.CreateInstance(pakType)
            ?? throw new InvalidOperationException("CATHODE.PAK2 could not be created.");
        if (TryLoadWithPath(pak, uiPakPath) || TryLoadWithoutPath(pak, uiPakPath)) return pak;
        throw new InvalidOperationException("Unable to load UI.PAK with CATHODE.PAK2.");
    }

    private static bool TryLoadWithPath(object pak, string uiPakPath)
    {
        var method = pak.GetType().GetMethod("Load", new[] { typeof(string) });
        if (method is null) return false;
        method.Invoke(pak, new object[] { uiPakPath });
        return true;
    }

    private static bool TryLoadWithoutPath(object pak, string uiPakPath)
    {
        var property = pak.GetType().GetProperty("Filepath");
        if (property is { CanWrite: true }) property.SetValue(pak, uiPakPath);
        var method = pak.GetType().GetMethod("Load", Type.EmptyTypes);
        if (method is null) return false;
        method.Invoke(pak, null);
        return true;
    }

    private static object? FindEntry(object entriesObject, string expectedPath)
    {
        if (entriesObject is not IEnumerable entries)
            throw new InvalidOperationException("CATHODE.PAK2 Entries is not enumerable.");
        var expected = NormalizePath(expectedPath);
        foreach (var entry in entries)
        {
            if (entry is null) continue;
            var path = GetEntryPath(entry);
            if (string.Equals(NormalizePath(path), expected, StringComparison.OrdinalIgnoreCase))
                return entry;
        }
        return null;
    }

    private static string GetEntryPath(object entry)
    {
        var names = new[] { "Filepath", "FilePath", "Path", "Filename", "FileName", "Name" };
        foreach (var name in names)
        {
            if (GetMemberValue(entry, name) is string value) return value;
        }
        return string.Empty;
    }

    private static void ReplaceEntryData(object entry, byte[] bytes)
    {
        if (TrySetByteMember(entry, bytes)) return;
        if (TryInvokeByteMethod(entry, bytes)) return;
        throw new InvalidOperationException("CathodeLib could not replace the UI.PAK entry data.");
    }

    private static bool TrySetByteMember(object entry, byte[] bytes)
    {
        var names = new[] { "Content", "Data", "Bytes", "RawData", "FileData" };
        foreach (var name in names)
        {
            if (TrySetByteProperty(entry, name, bytes) || TrySetByteField(entry, name, bytes))
                return true;
        }
        return false;
    }

    private static bool TrySetByteProperty(object entry, string name, byte[] bytes)
    {
        var property = entry.GetType().GetProperty(name,
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (property is not { CanWrite: true } || property.PropertyType != typeof(byte[]))
            return false;
        property.SetValue(entry, bytes);
        return true;
    }

    private static bool TrySetByteField(object entry, string name, byte[] bytes)
    {
        var field = entry.GetType().GetField(name,
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        if (field is null || field.FieldType != typeof(byte[])) return false;
        field.SetValue(entry, bytes);
        return true;
    }

    private static bool TryInvokeByteMethod(object entry, byte[] bytes)
    {
        var names = new[] { "SetData", "SetContent", "ReplaceData", "ReplaceContent" };
        foreach (var name in names)
        {
            var method = entry.GetType().GetMethod(name,
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                null, new[] { typeof(byte[]) }, null);
            if (method is null) continue;
            method.Invoke(entry, new object[] { bytes });
            return true;
        }
        return false;
    }

    private static void SavePak2(object pak, string uiPakPath)
    {
        var withPath = pak.GetType().GetMethod("Save", new[] { typeof(string) });
        if (withPath is not null) { ValidateSave(withPath.Invoke(pak, new object[] { uiPakPath })); return; }
        var withoutPath = pak.GetType().GetMethod("Save", Type.EmptyTypes);
        if (withoutPath is not null) { ValidateSave(withoutPath.Invoke(pak, null)); return; }
        throw new InvalidOperationException("CATHODE.PAK2 Save method was not found.");
    }

    private static void ValidateSave(object? result)
    {
        if (result is false) throw new InvalidOperationException("CathodeLib failed to save UI.PAK.");
    }

    private static object? GetMemberValue(object instance, string name)
    {
        var flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        var property = instance.GetType().GetProperty(name, flags);
        if (property is not null) return property.GetValue(instance);
        return instance.GetType().GetField(name, flags)?.GetValue(instance);
    }

    private static string NormalizePath(string path) => path
        .Replace('\\', '/')
        .Trim()
        .TrimStart('/');

    private static string BackupPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AIMP", "backups", "ui", "UI.PAK.original");
}
