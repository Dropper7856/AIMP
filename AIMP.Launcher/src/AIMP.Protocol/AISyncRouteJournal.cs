namespace AIMP.Protocol;

// Serialized by the execution stream owner. Only accepted AIEX operations bind
// routes; receiving bytes grants neither actor ownership nor native admission.
public sealed class AISyncRouteJournal
{
    private readonly record struct Command(uint A, uint B, uint Birth, uint Class,
        AISyncTransactionCapability Capability, ulong Owner, ulong Id);
    private sealed record Binding(uint Revision, uint Kind, uint Parts, bool Submitted, bool Terminal, bool Released, ulong Sequence, bool Authoritative);
    private readonly Dictionary<Command, Binding> _commands = new();
    private readonly Dictionary<AISyncRouteKey, byte[]> _routes = new();
    // One bounded value tombstone per already known command, never raw refs.
    private readonly Dictionary<Command, AISyncRouteGraph> _failures = new();
    public int BlockedCommands => _failures.Count;
    public bool IsBlocked(AISyncRouteKey key) => key.Scope==_scope && _failures.ContainsKey(Identity(key));
    // After Accepted, publishers MUST use these canonical bytes: an attributable
    // malformed/conflicting route is normalized into its command refusal.
    public byte[] Retained(AISyncRouteKey key) {
        if(key.Scope!=_scope)throw new ProtocolException("Foreign route scope.");
        if(_failures.TryGetValue(Identity(key),out var fault)) {
            if(fault.Key!=key)throw new ProtocolException("Foreign route refusal key.");
            return AISyncRouteCodec.Encode(fault);
        }
        return _routes[key].ToArray();
    }
    private AISyncExecutionJournalResult Block(AISyncRouteKey key,AISyncRouteFailure reason)
    {
        Expire(Identity(key));
        _failures.Add(Identity(key),new(key,new float[6],new uint[4],Array.Empty<AISyncRouteResource>(),Array.Empty<AISyncRouteNode>()){Failure=reason});
        return AISyncExecutionJournalResult.Accepted;
    }
    private readonly AISyncExecutionScope _scope;
    private readonly int _maxBytes, _maxRoutes;
    public bool RecoveryRequired { get; private set; }
    public int RetainedBytes { get; private set; }
    public int RouteCount => _routes.Count;

    public AISyncRouteJournal(AISyncExecutionScope scope, int maxBytes = 2 * 1024 * 1024, int maxRoutes = 2048)
    {
        AISyncExecutionCodec.RequireCurrentBinding(scope, scope);
        if (maxBytes is < 1 or > 2 * 1024 * 1024 || maxRoutes is < 1 or > 2048)
            throw new ArgumentOutOfRangeException(nameof(maxBytes));
        _scope = scope; _maxBytes = maxBytes; _maxRoutes = maxRoutes;
    }
    private static Command Identity(AISyncTransaction r) => new(r.EntityA, r.EntityB, r.Birth,
        r.CharacterClass, r.Capability, r.OwnerEpoch, r.CommandId);
    private static Command Identity(AISyncRouteKey k) => new(k.EntityA, k.EntityB, k.Birth,
        k.CharacterClass, k.Capability, k.OwnerEpoch, k.CommandId);

    // Call once for each newly accepted batch, never for a retry or duplicate.
    public bool Observe(AISyncExecutionBatch batch)
    {
        AISyncExecutionCodec.RequireCurrentBinding(batch.Scope, _scope);
        if (RecoveryRequired) return false;
        foreach (var r in batch.Records)
        {
            var id = Identity(r);
            if (!_commands.ContainsKey(id) && _commands.Count == 2048) { Fence(); return false; }
            _commands.TryGetValue(id, out var previous);
            var terminal = r.Opcode is AISyncTransactionOpcode.Cancel or AISyncTransactionOpcode.Stop
                or AISyncTransactionOpcode.Complete or AISyncTransactionOpcode.Release;
            if (previous?.Terminal == true && !terminal) { Fence(); return false; }
            var submitted = r.Opcode is AISyncTransactionOpcode.Submit or AISyncTransactionOpcode.Update;
            _commands[id] = new(r.ParameterRevision, r.NativeKind, r.Movement.Parts,
                submitted || previous?.Submitted == true, terminal,
                r.Opcode == AISyncTransactionOpcode.Release || previous?.Released == true, r.GlobalSequence,
                r.Movement.Version != 0 ? r.Movement.Navigation == AISyncMovementNavigation.HostRoute : previous?.Authoritative == true);
            if (terminal || previous?.Revision != r.ParameterRevision) Expire(id);
        }
        return true;
    }
    private void Expire(Command id)
    {
        foreach (var key in _routes.Keys.Where(k => Identity(k) == id).ToArray())
        { RetainedBytes -= _routes[key].Length; _routes.Remove(key); }
    }
    public bool Knows(AISyncRouteKey key) => !RecoveryRequired && key.Scope == _scope
        && _commands.TryGetValue(Identity(key), out var binding) && !binding.Terminal
        && binding.Submitted && binding.Authoritative && binding.Revision == key.ParameterRevision && binding.Kind is 1 or 3
        && key.Part < binding.Parts;

    public AISyncExecutionJournalResult Append(ReadOnlySpan<byte> bytes)
    {
        if (RecoveryRequired) return AISyncExecutionJournalResult.RecoveryRequired;
        var key = AISyncRouteCodec.DecodeKey(bytes);
        if (!Knows(key)) return AISyncExecutionJournalResult.Rejected;
        if(_failures.TryGetValue(Identity(key),out var existing))
            return existing.Key==key && AISyncRouteCodec.Encode(existing).AsSpan().SequenceEqual(bytes)
                ? AISyncExecutionJournalResult.Duplicate : AISyncExecutionJournalResult.Rejected;
        AISyncRouteGraph graph;
        try {graph=AISyncRouteCodec.Decode(bytes,key);}
        catch(ProtocolException) {return Block(key,AISyncRouteFailure.InvalidStream);}
        if(graph.Failure!=AISyncRouteFailure.None)return Block(key,graph.Failure);
        if (_routes.TryGetValue(key, out var previous))
        {
            if (previous.AsSpan().SequenceEqual(bytes)) return AISyncExecutionJournalResult.Duplicate;
            return Block(key,AISyncRouteFailure.InvalidStream);
        }
        if (_routes.Keys.Any(k => Identity(k) == Identity(key) && k.Part == key.Part && k.RouteRevision > key.RouteRevision))
            return AISyncExecutionJournalResult.Rejected;
        if (_routes.Count == _maxRoutes || bytes.Length > _maxBytes - RetainedBytes)
            return Block(key,AISyncRouteFailure.Capacity);
        _routes.Add(key, bytes.ToArray()); RetainedBytes += bytes.Length;
        return AISyncExecutionJournalResult.Accepted;
    }
    public void Fence() { RecoveryRequired = true; _routes.Clear(); _failures.Clear(); _commands.Clear(); RetainedBytes = 0; }
    // The embedding stream calls this only after its native prefix ACK.
    public void Checkpoint(ulong through)
    {
        foreach (var pair in _commands.Where(p => p.Value.Released && p.Value.Sequence <= through).ToArray())
        { Expire(pair.Key); _failures.Remove(pair.Key); _commands.Remove(pair.Key); }
    }
}
