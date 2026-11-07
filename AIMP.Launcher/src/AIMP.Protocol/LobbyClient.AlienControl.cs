namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private readonly object _alienControlGate=new();
    private ulong _lastAlienControlHostSequence,_lastAlienPoseSequence;
    private readonly AlienActionJournal _alienActions=new();
    private AlienControlLease? _alienControlLease;
    public event EventHandler<AlienControlCommit>? AlienControlCommitted;
    public event EventHandler<AlienControlLease>? AlienControlLeaseChanged;
    public event EventHandler<AlienControlPose>? AlienControlPoseReceived;
    public event EventHandler<AlienActionResolution>? AlienActionResolved;
    public Task SendAlienControlAsync(AlienControlIntent intent,CancellationToken cancellationToken=default)
    {
        AlienControlProtocol.Validate(intent);
        var snapshot=Snapshot;
        if(intent.SessionId!=SessionId || intent.AlienPeerId!=PeerId ||
            snapshot?.AlienVsSurvivors!=true || snapshot.AlienPeerId!=PeerId)
            throw new ProtocolException("This participant is not the Alien decision maker.");
        lock(_alienControlGate)
            if(_alienControlLease is not {Active:true} lease || lease.Epoch!=intent.Epoch ||
                lease.LevelGeneration!=intent.LevelGeneration)return Task.CompletedTask;
        return _connection.SendAsync(MessageType.AlienControlProposal,JsonPayload.Serialize(intent),cancellationToken).AsTask();
    }
    public Task ReportAlienReadyAsync(AlienControlReady ready,CancellationToken cancellationToken=default)
    {
        AlienControlAuthorityValidation.Validate(ready);
        var snapshot=Snapshot;
        if(ready.SessionId!=SessionId || ready.PeerId!=PeerId || snapshot?.AlienVsSurvivors!=true)
            throw new ProtocolException("Readiness is not bound to this participant.");
        if(snapshot.State!="Starting" || ready.LevelGeneration!=snapshot.LevelGeneration ||
            ready.AlienPeerId!=snapshot.AlienPeerId)return Task.CompletedTask;
        return _connection.SendAsync(MessageType.AlienControlReady,JsonPayload.Serialize(ready),cancellationToken).AsTask();
    }
    private bool AlienRoundCurrent(Guid session,int generation,ulong owner)
    {
        var current=Snapshot;
        return session==SessionId && current?.AlienVsSurvivors==true && current.State=="Starting" &&
            current.AlienPeerId==owner && current.LevelGeneration==generation;
    }
    private void HandleAlienLease(ReadOnlyMemory<byte> bytes)
    {
        var lease=JsonPayload.Deserialize<AlienControlLease>(bytes.Span);
        AlienControlAuthorityValidation.Validate(lease);
        if(!AlienRoundCurrent(lease.SessionId,lease.LevelGeneration,lease.AlienPeerId))return;
        lock(_alienControlGate)
        {
            var old=_alienControlLease;
            if(old is not null && old.SessionId==lease.SessionId && old.LevelGeneration==lease.LevelGeneration)
            {
                if(lease.Epoch<old.Epoch || (lease.Epoch==old.Epoch && !old.Active && lease.Active))return;
                if(lease.Epoch==old.Epoch && (lease.EntityA!=old.EntityA || lease.EntityB!=old.EntityB ||
                    !lease.Bindings.SequenceEqual(old.Bindings)))
                    throw new ProtocolException("Possession identity changed inside an Alien epoch.");
            }
            if(old is null || old.Epoch!=lease.Epoch || old.LevelGeneration!=lease.LevelGeneration)
            { _lastAlienControlHostSequence=_lastAlienPoseSequence=0;_alienActions.Clear(); }
            _alienControlLease=lease;
        }
        AlienControlLeaseChanged?.Invoke(this,lease);
    }
    private void HandleAlienControlCommitted(ReadOnlyMemory<byte> bytes)
    {
        var value=JsonPayload.Deserialize<AlienControlCommit>(bytes.Span);
        AlienControlProtocol.Validate(value);var intent=value.Intent;
        if(!AlienRoundCurrent(intent.SessionId,intent.LevelGeneration,intent.AlienPeerId))return;
        lock(_alienControlGate)
        {
            if(_alienControlLease is not {Active:true} lease || lease.Epoch!=intent.Epoch ||
                lease.LevelGeneration!=intent.LevelGeneration || value.HostSequence<=_lastAlienControlHostSequence)return;
            if(!_alienActions.Issue(value))throw new ProtocolException("Alien action journal exhausted or duplicated.");
            _lastAlienControlHostSequence=value.HostSequence;
        }
        AlienControlCommitted?.Invoke(this,value);
    }
    private void HandleAlienActionResolution(ReadOnlyMemory<byte> bytes)
    {
        var r=JsonPayload.Deserialize<AlienActionResolution>(bytes.Span);
        AlienActionValidation.Validate(r);
        if(!AlienRoundCurrent(r.SessionId,r.LevelGeneration,r.AlienPeerId))return;
        lock(_alienControlGate)
        {
            if(_alienControlLease is not {Active:true} l || r.Epoch!=l.Epoch ||
                r.EntityA!=l.EntityA || r.EntityB!=l.EntityB || !_alienActions.Accept(r))return;
        }
        AlienActionResolved?.Invoke(this,r);
    }
    private void HandleAlienPose(ReadOnlyMemory<byte> bytes)
    {
        var pose=JsonPayload.Deserialize<AlienControlPose>(bytes.Span);
        AlienControlAuthorityValidation.Validate(pose);
        if(!AlienRoundCurrent(pose.SessionId,pose.LevelGeneration,pose.AlienPeerId))return;
        lock(_alienControlGate)
        {
            if(_alienControlLease is not {Active:true} lease || lease.Epoch!=pose.Epoch ||
                lease.LevelGeneration!=pose.LevelGeneration || lease.EntityA!=pose.EntityA || lease.EntityB!=pose.EntityB ||
                pose.Sequence<=_lastAlienPoseSequence || pose.HostSequence>_lastAlienControlHostSequence)return;
            _lastAlienPoseSequence=pose.Sequence;
        }
        AlienControlPoseReceived?.Invoke(this,pose);
    }
}
