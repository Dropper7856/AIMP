using AIMP.Protocol;
namespace AIMP.Launcher;
internal sealed record NativeAlienPvP(Guid ConnectionIdentity,int ProcessId,AlienPvPNativeFrame Frame);
