using AIMP.Protocol;

namespace AIMP.Launcher;

internal readonly record struct AISyncShadowBinding(
    LobbyHost? Host, Guid Session, int WorldGeneration, int NativeGeneration,
    int ProcessId, Guid ConnectionIdentity, long Epoch, bool Active);
