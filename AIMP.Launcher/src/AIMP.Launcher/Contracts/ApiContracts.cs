namespace AIMP.Launcher.Contracts;

public sealed record GamePreparationDto(string JobId, string GameDirectory, string State,
    string Stage, int Percentage, int CompletedLevels, int TotalLevels, string CurrentLevel,
    string Message, string Error, DateTimeOffset? StartedAtUtc, DateTimeOffset? CompletedAtUtc);

public sealed record LauncherStatusResponse(
    string LauncherVersion,
    NativeRuntimeStatus Runtime,
    BridgeStatus Bridge,
    MultiplayerStatus Multiplayer,
    LocalPlayerStatus LocalPlayer,
    IReadOnlyList<PerformanceModuleStatus> PerformanceModules,
    string LauncherLogPath);

public sealed record NativeRuntimeStatus(
    bool AiRunning,
    int ProcessId,
    string ExecutablePath,
    string ModuleBase,
    string DllPath,
    bool DllInjected,
    bool IpcConnected,
    string NativeDllVersion,
    bool EngineHooksInstalled,
    bool MainHookInstalled,
    bool FallbackHookInstalled,
    bool ProducerHookInstalled,
    bool EngineThreadReady,
    int EngineThreadId,
    string EngineThreadSource,
    DateTimeOffset? FirstEngineCallbackUtc,
    DateTimeOffset? LastEngineCallbackUtc,
    ulong FirstEngineCallbackCounter,
    ulong LastEngineCallbackCounter,
    int PendingCommandCount,
    string LastCommand,
    string LastCommandResult,
    string LastNativeException,
    int LevelGeneration,
    string NativeLogPath,
    RemoteCharacterStatus RemoteCharacter);

public sealed record RemoteCharacterStatus(
    string State,
    uint SpawnKey,
    string CharacterBase,
    string CharacterRecord,
    bool CharacterCreateHookInstalled);

public sealed record BridgeStatus(bool Running, bool Connected, int ConnectedClients);

public sealed record MultiplayerStatus(
    string NetworkState,
    bool IsHost,
    bool Connected,
    bool LocalGameplayReady,
    bool RemoteGameplayReady,
    string RemotePlayerName);

public sealed record LocalPlayerStatus(
    bool Available,
    float? X,
    float? Y,
    float? Z,
    float? Yaw,
    string Message,
    DateTimeOffset UpdatedAtUtc);

public sealed record PerformanceModuleStatus(
    string Module,
    string Label,
    string Description,
    bool Enabled);

public sealed record SettingsResponse(
    string GameDirectory,
    string PlayerName,
    string HostAddress,
    int Port,
    Guid PlayerId,
    bool WorldSyncEnabled,
    bool WorldSync3EmptyCallbackEnabled,
    string SettingsPath);

public sealed record UpdateSettingsRequest(
    string? GameDirectory,
    string? PlayerName,
    string? HostAddress,
    int Port,
    bool WorldSyncEnabled = true,
    bool WorldSync3EmptyCallbackEnabled = false);

public sealed record SettingsValidationResponse(
    bool Valid,
    string Error,
    string? NormalizedGameDirectory,
    string? NormalizedPlayerName,
    string? NormalizedHostAddress,
    int? NormalizedPort);

public sealed record GameLaunchResponse(int ProcessId);

public sealed record InjectionResponse(NativeRuntimeStatus Runtime);

public sealed record DiagnosticsExportRequest(string? OutputPath);

public sealed record DiagnosticsExportResponse(string ArchivePath);

public sealed record LogResponse(
    string LogPath,
    bool RecordingEnabled,
    IReadOnlyList<string> Entries);

public sealed record CommandDefinitionResponse(
    string Name,
    string Label,
    string Description,
    string Target,
    IReadOnlyList<CommandParameterResponse> Parameters);

public sealed record CommandParameterResponse(
    string Key,
    string Label,
    string Kind,
    string DefaultValue,
    string Description,
    decimal Minimum,
    decimal Maximum,
    decimal Increment,
    int DecimalPlaces,
    IReadOnlyList<CommandChoiceResponse> Choices,
    int MaximumUtf8Bytes);

public sealed record CommandChoiceResponse(string Label, string Value);

public sealed record CommandExecutionRequest(IReadOnlyDictionary<string, string>? Arguments);

public sealed record CommandExecutionResponse(
    Guid RequestId,
    string Command,
    bool Executed,
    string StatusCode,
    string Message,
    bool Queued,
    string DisplayText);

public sealed record PerformanceUpdateRequest(bool Enabled);

public sealed record CharacterBaseRequest(string? Ecx);

public sealed record CharacterBaseResponse(
    string Ecx,
    string CharacterBase,
    bool Is16ByteAligned);

public sealed record WorldLevelResponse(
    string DisplayName,
    string RelativePath,
    string LevelDirectory,
    bool CanCompile,
    string ValidationMessage);

public sealed record WorldLevelListResponse(
    string ProductionDirectory,
    IReadOnlyList<WorldLevelResponse> Levels);

public sealed record StartWorldManifestCompilationRequest(IReadOnlyList<string>? RelativePaths);

public sealed record WorldManifestCompilationJobResponse(
    Guid Id,
    string Status,
    int OverallPercentage,
    int CompletedLevels,
    int TotalLevels,
    int Created,
    int CacheHits,
    int Failed,
    DateTimeOffset CreatedAtUtc,
    DateTimeOffset? CompletedAtUtc,
    IReadOnlyList<WorldManifestCompilationItemResponse> Items);

public sealed record WorldManifestCompilationItemResponse(
    string RelativePath,
    string DisplayName,
    string Status,
    int Percentage,
    string Stage,
    string? MapContentId,
    int? EntityCount,
    string? BinaryPath,
    bool? CacheHit,
    string? Error);

public sealed record FileSystemDriveResponse(
    string Id,
    string Name,
    string Path,
    string Type,
    bool Available,
    long? TotalBytes,
    long? FreeBytes);

public sealed record FileSystemEntryResponse(
    string Id,
    string Name,
    string Path,
    string Type,
    long? SizeBytes,
    DateTimeOffset ModifiedAtUtc,
    string Permissions,
    string? Extension,
    bool IsGameExecutable);

public sealed record DirectoryBrowseResponse(
    string CurrentPath,
    string? ParentPath,
    string Permissions,
    IReadOnlyList<FileSystemEntryResponse> Entries);

public sealed record ValidateGameExecutableRequest(string? ExecutablePath);

public sealed record ValidateGameExecutableResponse(
    string GameDirectory,
    string ExecutablePath);
