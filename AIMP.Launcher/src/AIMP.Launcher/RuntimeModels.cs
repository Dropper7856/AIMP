using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed record AiProcessTarget(
    int ProcessId,
    string ExecutablePath,
    nint ModuleBase,
    long StartTimeUtcTicks,
    bool HasMainWindow);

internal sealed record NativeRuntimeSnapshot(
    bool AiRunning,
    int ProcessId,
    string ExecutablePath,
    nint ModuleBase,
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
    string RemoteCharacterState = RemoteCharacterStateNames.None,
    uint RemoteCharacterSpawnKey = 0,
    uint RemoteCharacterBase = 0,
    uint RemoteCharacterRecord = 0,
    bool CharacterCreateHookInstalled = false,
    ulong LocalCharacterGeneration = 0,
    ulong LocalCharacterSerial = 0,
    uint LocalCharacterBase = 0,
    uint LocalCharacterRuntimeId = 0,
    uint LocalCharacterOrigin = 0,
    Guid LocalCharacterConnectionIdentity = default,
    int RemoteCharacterGeneration = 0,
    ulong RemoteCharacterCreationSerial = 0,
    uint RemoteCharacterRuntimeId = 0,
    Guid RemoteCharacterConnectionIdentity = default)
{
    internal static NativeRuntimeSnapshot Empty { get; } = new(
        false, 0, string.Empty, 0, string.Empty, false, false, string.Empty,
        false, false, false, false, false, 0, "Not detected", null, null, 0, 0, 0,
        string.Empty, string.Empty, string.Empty, 0, string.Empty);
}

internal sealed record EngineStateUpdate(
    bool HooksInstalled,
    bool MainHookInstalled,
    bool FallbackHookInstalled,
    bool ProducerHookInstalled,
    bool Ready,
    int ThreadId,
    string Source,
    ulong FirstCallbackCounter,
    ulong LastCallbackCounter,
    int PendingCount,
    int Generation);

internal sealed record RuntimeHandshakeResult(
    BuildInfoPayload BuildInfo,
    NetZReadyPayload Ready,
    string NativeDllVersion);

internal sealed record PendingIpcResponse(
    MessageType ExpectedType,
    TaskCompletionSource<AimpFrame> Completion);

internal sealed record InjectorResult(bool Success, string Code, string Message);
