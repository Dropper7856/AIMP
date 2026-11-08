namespace AIMP.Protocol;

public enum AlienActionPhase : uint { None, Started, Completed, Rejected, Failed, Cancelled, Preparing }
public enum AlienActionReason : uint
{ None, NativeBusy, Cooldown, NoTarget, NotReady, NativeUnavailable, NativeRefused,
  NoVent, AmbiguousVent, WrongStage, Stale, Timeout, Fault, Occluded }

// Only the Host's certified native actor can produce a resolution. No raw
// pointer, local vent index or client-proposed victim can enter this contract.
// XYZ = actual vent entrance; ResolvedXYZ = its exit. Attack uses the selected
// victim's copied position. Selection is immutable after the Started receipt.
public sealed record AlienActionResolution(Guid SessionId,int LevelGeneration,ulong AlienPeerId,
    ulong Epoch,ulong InputSequence,ulong HostSequence,AlienAction Action,ulong Revision,
    AlienActionPhase Phase,AlienActionReason Reason,uint EntityA,uint EntityB,
    ulong TargetPeer,uint TargetA,uint TargetB,uint Selector,float X,float Y,float Z,
    float ResolvedX,float ResolvedY,float ResolvedZ);

public static class AlienActionValidation
{
    public static bool Next(AlienActionPhase previous,AlienActionPhase next) => previous switch {
        AlienActionPhase.None => next is AlienActionPhase.Preparing or AlienActionPhase.Started or AlienActionPhase.Rejected or AlienActionPhase.Failed,
        AlienActionPhase.Preparing => next is AlienActionPhase.Started or AlienActionPhase.Rejected or AlienActionPhase.Failed or AlienActionPhase.Cancelled,
        AlienActionPhase.Started => next is AlienActionPhase.Completed or AlienActionPhase.Failed or AlienActionPhase.Cancelled,
        _ => false
    };
    public static bool SameSelection(AlienActionResolution a,AlienActionResolution b) =>
        a.TargetPeer==b.TargetPeer && a.TargetA==b.TargetA && a.TargetB==b.TargetB && (a.Phase==AlienActionPhase.Preparing || a.Selector==b.Selector) &&
        a.X==b.X && a.Y==b.Y && a.Z==b.Z && a.ResolvedX==b.ResolvedX && a.ResolvedY==b.ResolvedY && a.ResolvedZ==b.ResolvedZ;
    public static void Validate(AlienActionResolution? r)
    {
        if(r is null || r.SessionId==Guid.Empty || r.LevelGeneration<1 || r.AlienPeerId==0 || r.Epoch==0 ||
            r.InputSequence==0 || r.HostSequence==0 || r.Revision==0 || (r.EntityA==0 && r.EntityB==0) ||
            r.Action is not (AlienAction.Attack or AlienAction.EnterVent or AlienAction.ExitVent) ||
            (uint)r.Phase is <1 or >6 || (uint)r.Reason>13)
            throw new ProtocolException("Invalid Alien action resolution identity/state.");
        foreach(var v in new[]{r.X,r.Y,r.Z,r.ResolvedX,r.ResolvedY,r.ResolvedZ})
            if(!float.IsFinite(v) || Math.Abs(v)>100000)throw new ProtocolException("Invalid action coordinate.");
        if((r.Phase is AlienActionPhase.Preparing or AlienActionPhase.Started or AlienActionPhase.Completed) && r.Reason!=AlienActionReason.None)
            throw new ProtocolException("Successful action has a failure reason.");
        if((r.Phase is AlienActionPhase.Rejected or AlienActionPhase.Failed or AlienActionPhase.Cancelled) && r.Reason==AlienActionReason.None)
            throw new ProtocolException("Unsuccessful action must explain its reason.");
        if(r.Action==AlienAction.Attack && (r.Phase is AlienActionPhase.Preparing or AlienActionPhase.Started or AlienActionPhase.Completed) &&
            (r.TargetPeer==0 || r.TargetPeer==r.AlienPeerId || (r.TargetA==0 && r.TargetB==0) || r.Selector>65535))
            throw new ProtocolException("Attack has no certified survivor/selector.");
        if(r.Phase==AlienActionPhase.Preparing && r.Action!=AlienAction.Attack)
            throw new ProtocolException("Only attack uses the native availability phase.");
        if(r.Action!=AlienAction.Attack && (r.TargetPeer!=0 || r.TargetA!=0 || r.TargetB!=0 || r.Selector!=0))
            throw new ProtocolException("Vent resolution cannot address a victim or local vent slot.");
    }
}

// Bounded journal separate from steering. A rejected newer click cannot erase
// an older ongoing native action's completion; duplicates never renew timers.
public sealed class AlienActionJournal
{
    private sealed record Entry(AlienControlCommit Command,AlienActionResolution? Last,long IssuedAt,long UpdatedAt);
    private readonly SortedDictionary<ulong,Entry> _entries=new();
    public const int Capacity=64;
    public void Clear()=>_entries.Clear();
    public bool CanIssue => _entries.Count<Capacity || _entries.Values.Any(e=>e.Last is {Phase:not (AlienActionPhase.Started or AlienActionPhase.Preparing)});
    public bool Stalled(long now) => _entries.Values.Any(e => {
        if(e.Last is {Phase:not (AlienActionPhase.Preparing or AlienActionPhase.Started)})return false;
        // Preparing covers the Host's retail melee window (native 10s); a
        // shorter limit revoked the whole epoch while the brain was still armed.
        var limit=e.Last?.Phase switch {AlienActionPhase.Started=>35000L,AlienActionPhase.Preparing=>14000L,_=>2500L};
        return now<e.UpdatedAt || now-e.UpdatedAt>limit;
    });
    public bool Issue(AlienControlCommit commit,long now=0)
    {
        AlienControlProtocol.Validate(commit);
        if(commit.Intent.Action is not (AlienAction.Attack or AlienAction.EnterVent or AlienAction.ExitVent))return true;
        if(_entries.ContainsKey(commit.HostSequence))return false;
        if(_entries.Count==Capacity)
        {
            var victim=_entries.FirstOrDefault(e=>e.Value.Last is {Phase:not (AlienActionPhase.Started or AlienActionPhase.Preparing)});
            if(victim.Key==0)return false;
            _entries.Remove(victim.Key);
        }
        _entries.Add(commit.HostSequence,new(commit,null,now,now));return true;
    }
    public bool Contains(AlienActionResolution r) =>
        _entries.TryGetValue(r.HostSequence,out var e) && e.Last==r;
    public bool Accept(AlienActionResolution r,long now=0)
    {
        AlienActionValidation.Validate(r);
        if(!_entries.TryGetValue(r.HostSequence,out var e))return false;
        var i=e.Command.Intent;var p=e.Last;
        if(r.SessionId!=i.SessionId || r.LevelGeneration!=i.LevelGeneration || r.AlienPeerId!=i.AlienPeerId ||
            r.Epoch!=i.Epoch || r.InputSequence!=i.Sequence || r.Action!=i.Action ||
            !AlienActionValidation.Next(p?.Phase??AlienActionPhase.None,r.Phase) ||
            r.Revision!=(p?.Revision??0)+1 || (p is not null && !AlienActionValidation.SameSelection(p,r)))return false;
        _entries[r.HostSequence]=e with {Last=r,UpdatedAt=now};return true;
    }
}
