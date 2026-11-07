namespace AIMP.Protocol;

// Reliable only across the local IPC queues; peer sockets retain their own
// authenticated ordering. These IDs must never replace gameplay sequences.
public static class AlienPvPDelivery
{
    public const long RetryMs=75, DeadlineMs=2000;
    // Central direction contract used by DllIpcSession, including action
    // resolutions in BOTH directions. This guards the v416 whitelist defect.
    public static bool NativeToManaged(AlienPvPNativeKind kind) => kind is
        AlienPvPNativeKind.Status or AlienPvPNativeKind.Input or AlienPvPNativeKind.Pose or
        AlienPvPNativeKind.ActionResult or AlienPvPNativeKind.Resolution or AlienPvPNativeKind.Acknowledge;
    public static bool ManagedToNative(AlienPvPNativeKind kind) => kind is
        AlienPvPNativeKind.Activate or AlienPvPNativeKind.Commit or AlienPvPNativeKind.Correction or
        AlienPvPNativeKind.Stop or AlienPvPNativeKind.Resolution or AlienPvPNativeKind.Acknowledge;
    public static bool Reliable(AlienPvPNativeFrame f) =>
        f.Kind is AlienPvPNativeKind.Activate or AlienPvPNativeKind.Stop or
            AlienPvPNativeKind.Resolution or AlienPvPNativeKind.ActionResult ||
        ((f.Kind is AlienPvPNativeKind.Input or AlienPvPNativeKind.Commit) && f.Action!=AlienAction.None);
    public static AlienPvPNativeFrame Acknowledgement(AlienPvPNativeFrame f) => new()
    {Kind=AlienPvPNativeKind.Acknowledge,Scope=f.Scope,Epoch=f.Epoch,DeliveryId=f.DeliveryId};
}
public sealed class AlienPvPDeliveryInbox
{
    public enum Verdict { Next, Duplicate, Gap }
    private ulong _last;
    public Verdict Inspect(ulong id) => id==0 || (id>>32)!=(_last>>32)?Verdict.Gap:id<=_last?Verdict.Duplicate:
        _last!=ulong.MaxValue && id==_last+1?Verdict.Next:Verdict.Gap;
    public bool Commit(ulong id)
    {if(Inspect(id)!=Verdict.Next)return false;_last=id;return true;}
    public void Clear(ulong prefix=0)=>_last=prefix;
}
public sealed class AlienPvPDeliveryOutbox
{
    private sealed record Entry(AlienPvPNativeFrame Frame,long Created,long Attempt,bool Sent);
    private readonly Queue<Entry> _queue=new();
    private Entry? _head;
    private ulong _next;
    public int Count=>_queue.Count+(_head is null?0:1);
    private void Head(){if(_head is null && _queue.TryDequeue(out var h))_head=h;}
    public void Clear(ulong prefix=0){_queue.Clear();_head=null;_next=prefix;}
    public bool Enqueue(AlienPvPNativeFrame frame,long now)
    {
        AlienPvPNativeCodec.Validate(frame);
        if(!AlienPvPDelivery.Reliable(frame))return false;
        // Status renewals may ask to resend the same activation before its ACK.
        bool Same(Entry e)=>e.Frame.Kind==frame.Kind && e.Frame.Scope==frame.Scope &&
            e.Frame.Epoch==frame.Epoch && e.Frame.Sequence==frame.Sequence &&
            e.Frame.HostSequence==frame.HostSequence && e.Frame.ResolutionRevision==frame.ResolutionRevision;
        var duplicate=(_head is not null && Same(_head))?_head:_queue.FirstOrDefault(Same);
        if(duplicate is not null)
            return (duplicate.Frame with {DeliveryId=0,SampledAtMs=0}) == (frame with {DeliveryId=0,SampledAtMs=0});
        if(Count>=64 || (_next & uint.MaxValue)==uint.MaxValue)return false;
        _queue.Enqueue(new(frame with {DeliveryId=++_next,SampledAtMs=checked((ulong)now)},now,0,false));
        Head();return true;
    }
    public AlienPvPNativeFrame? Due(long now)
    {
        Head();if(_head is not { } h)return null;
        if(h.Sent && now>=h.Attempt && now-h.Attempt<AlienPvPDelivery.RetryMs)return null;
        _head=h with {Attempt=now,Sent=true};return h.Frame;
    }
    public bool Expired(long now)
    {Head();return _head is { } h && (now<h.Created || now-h.Created>AlienPvPDelivery.DeadlineMs);}
    public bool Acknowledge(AlienPvPNativeFrame f)
    {
        Head();if(f.Kind!=AlienPvPNativeKind.Acknowledge || _head is not { } h ||
            h.Frame.Scope!=f.Scope || h.Frame.Epoch!=f.Epoch || h.Frame.DeliveryId!=f.DeliveryId)return false;
        _head=null;Head();return true;
    }
}
