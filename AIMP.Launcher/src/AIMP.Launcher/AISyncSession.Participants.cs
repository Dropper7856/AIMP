using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class AISyncSession
{
    private readonly Dictionary<ulong,AISyncParticipantEvidence> _participantEvidence=new();
    private AISyncParticipantEvidence? _localEvidence;

    internal void SetLocalIncarnation(Guid session,ulong peer,uint incarnation,NativeRuntimeSnapshot runtime,Guid connection)
    {
        var evidence=AISyncParticipantEvidence.FromLocal(incarnation,runtime,connection);
        lock(_gate)
        {
            if(_disposed||_session!=session||_localPeer!=peer||_localEvidence==evidence)return;
            _localEvidence=evidence;++_participantRevision;ScheduleParticipantRefreshLocked();
        }
    }
    internal void ObserveParticipantIncarnation(ulong peer,uint incarnation)
    {
        lock(_gate)
        {
            if(!_participantEvidence.TryGetValue(peer,out var evidence)||evidence.Binding.Incarnation==incarnation)return;
            _participantEvidence.Remove(peer);++_participantRevision;ScheduleParticipantRefreshLocked();
        }
    }
    private Dictionary<ulong,AISyncParticipantBinding> CurrentParticipantEvidence(NativeRuntimeSnapshot runtime) =>
        _participantEvidence.Where(p=>p.Value.Matches(runtime,_controller.AISyncConnectionIdentity,_nativeGeneration))
            .ToDictionary(p=>p.Key,p=>p.Value.Binding);
}
