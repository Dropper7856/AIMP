using AIMP.Protocol;
namespace AIMP.Launcher;

internal sealed record NativeAISyncExecution(Guid ConnectionIdentity, int ProcessId,
    MessageType Type, ReadOnlyMemory<byte> Bytes);
