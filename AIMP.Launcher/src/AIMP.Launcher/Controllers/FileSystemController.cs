using AIMP.Launcher.Contracts;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/filesystem")]
public sealed class FileSystemController : ControllerBase
{
    [HttpGet("drives")]
    [ProducesResponseType<IReadOnlyList<FileSystemDriveResponse>>(StatusCodes.Status200OK)]
    public ActionResult<IReadOnlyList<FileSystemDriveResponse>> GetDrives()
    {
        var drives = DriveInfo.GetDrives()
            .OrderBy(drive => drive.Name, StringComparer.OrdinalIgnoreCase)
            .Select(MapDrive)
            .ToArray();
        return Ok(drives);
    }

    [HttpGet("directory")]
    [ProducesResponseType<DirectoryBrowseResponse>(StatusCodes.Status200OK)]
    public ActionResult<DirectoryBrowseResponse> GetDirectory([FromQuery] string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("A directory path is required.", nameof(path));

        var directory = new DirectoryInfo(Path.GetFullPath(path.Trim()));
        if (!directory.Exists)
            throw new DirectoryNotFoundException($"Directory '{directory.FullName}' was not found.");

        var entries = directory.EnumerateFileSystemInfos()
            .OrderByDescending(entry => entry is DirectoryInfo)
            .ThenBy(entry => entry.Name, StringComparer.OrdinalIgnoreCase)
            .Select(MapEntry)
            .ToArray();
        return Ok(new DirectoryBrowseResponse(
            directory.FullName,
            directory.Parent?.FullName,
            ResolvePermissions(directory.Attributes),
            entries));
    }

    [HttpPost("validate-game-executable")]
    [ProducesResponseType<ValidateGameExecutableResponse>(StatusCodes.Status200OK)]
    public ActionResult<ValidateGameExecutableResponse> ValidateGameExecutable(
        ValidateGameExecutableRequest request)
    {
        if (string.IsNullOrWhiteSpace(request.ExecutablePath))
            throw new ArgumentException("Select AI.exe.", nameof(request));

        var executable = Path.GetFullPath(request.ExecutablePath.Trim());
        if (!string.Equals(Path.GetFileName(executable), "AI.exe", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("The selected file must be AI.exe.", nameof(request));
        if (!System.IO.File.Exists(executable))
            throw new FileNotFoundException("The selected AI.exe does not exist.", executable);

        var gameDirectory = Path.GetDirectoryName(executable)
            ?? throw new ArgumentException("The AI.exe directory could not be resolved.", nameof(request));
        if (!GameInstallation.TryResolve(gameDirectory, out var installation, out var error))
            throw new ArgumentException(error, nameof(request));
        return Ok(new ValidateGameExecutableResponse(
            installation.GameDirectory,
            installation.ExecutablePath));
    }

    private static FileSystemDriveResponse MapDrive(DriveInfo drive)
    {
        try
        {
            var available = drive.IsReady;
            return new FileSystemDriveResponse(
                drive.Name.ToUpperInvariant(),
                available && !string.IsNullOrWhiteSpace(drive.VolumeLabel)
                    ? drive.VolumeLabel
                    : drive.Name,
                drive.RootDirectory.FullName,
                drive.DriveType.ToString().ToUpperInvariant(),
                available,
                available ? drive.TotalSize : null,
                available ? drive.AvailableFreeSpace : null);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return new FileSystemDriveResponse(
                drive.Name.ToUpperInvariant(),
                drive.Name,
                drive.Name,
                drive.DriveType.ToString().ToUpperInvariant(),
                false,
                null,
                null);
        }
    }

    private static FileSystemEntryResponse MapEntry(FileSystemInfo entry)
    {
        var file = entry as FileInfo;
        var isGameExecutable = file is not null
            && string.Equals(file.Name, "AI.exe", StringComparison.OrdinalIgnoreCase);
        return new FileSystemEntryResponse(
            entry.FullName.ToUpperInvariant(),
            entry.Name,
            entry.FullName,
            entry is DirectoryInfo ? "DIRECTORY" : "FILE",
            file?.Length,
            entry.LastWriteTimeUtc,
            ResolvePermissions(entry.Attributes),
            file?.Extension.TrimStart('.') is { Length: > 0 } extension ? extension : null,
            isGameExecutable);
    }

    private static string ResolvePermissions(FileAttributes attributes) =>
        (attributes & FileAttributes.ReadOnly) != 0 ? "READ_ONLY" : "READ_WRITE";
}
