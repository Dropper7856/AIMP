namespace AIMP.Launcher;

// The actual delivering IPC connection stamps these values. A delayed callback
// from a replaced session cannot acquire the new connection's incarnation.
internal sealed record NativeAISyncObservation(
    Guid ConnectionIdentity, int ProcessId, ReadOnlyMemory<byte> Bytes);
