using AIMP.Protocol;

namespace AIMP.Launcher;

// Frozen at the completed, correlated spawn (or current THE_PLAYER capture).
// Reconfiguration must not discover a new native identity at an old address.
internal sealed record AISyncParticipantEvidence(AISyncParticipantBinding Binding,
    int NativeGeneration,int ProcessId,Guid ConnectionIdentity)
{
    internal bool Matches(NativeRuntimeSnapshot runtime,Guid connection,int generation) =>
        runtime.IpcConnected && connection!=Guid.Empty && ConnectionIdentity==connection
        && ProcessId==runtime.ProcessId && NativeGeneration==generation && runtime.LevelGeneration==generation;

    internal static AISyncParticipantEvidence? FromLocal(uint incarnation,NativeRuntimeSnapshot runtime,Guid connection) =>
        incarnation!=0 && NativeCharacterIncarnationBinding.MatchesCurrent(runtime,connection,runtime.LevelGeneration)
        ? new(new(runtime.LocalCharacterBase,incarnation,runtime.LocalCharacterRuntimeId,runtime.LocalCharacterSerial),
            runtime.LevelGeneration,runtime.ProcessId,connection) : null;

    internal static AISyncParticipantEvidence? FromSpawn(uint incarnation,uint character,uint spawnKey,
        NativeRuntimeSnapshot runtime,Guid connection) =>
        incarnation!=0 && runtime.IpcConnected && runtime.ProcessId>0 && connection!=Guid.Empty
        && runtime.RemoteCharacterConnectionIdentity==connection && runtime.RemoteCharacterState==RemoteCharacterStateNames.Ready
        && runtime.RemoteCharacterBase==character && runtime.RemoteCharacterSpawnKey==spawnKey
        && runtime.RemoteCharacterCreationSerial!=0 && runtime.RemoteCharacterRuntimeId!=0
        && runtime.LevelGeneration>0 && runtime.RemoteCharacterGeneration==runtime.LevelGeneration
        ? new(new(character,incarnation,runtime.RemoteCharacterRuntimeId,runtime.RemoteCharacterCreationSerial),
            runtime.LevelGeneration,runtime.ProcessId,connection) : null;
}
