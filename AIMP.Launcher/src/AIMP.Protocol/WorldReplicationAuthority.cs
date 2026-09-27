using System.Buffers.Binary;

namespace AIMP.Protocol;

public readonly record struct WorldAuthorityContextIdentity(
    Guid SessionId,
    int WorldGeneration,
    WorldManifestIdentity Manifest,
    string ManifestSha256,
    ulong Epoch)
{
    public bool Matches(WorldAuthorityContextIdentity other) =>
        SessionId == other.SessionId
        && WorldGeneration == other.WorldGeneration
        && Manifest == other.Manifest
        && string.Equals(ManifestSha256, other.ManifestSha256,
            StringComparison.OrdinalIgnoreCase)
        && Epoch == other.Epoch;

    public bool Matches(WorldNetworkContext context) =>
        SessionId == context.SessionId
        && WorldGeneration == context.WorldGeneration
        && Manifest == context.Manifest
        && string.Equals(ManifestSha256, context.ManifestSha256,
            StringComparison.OrdinalIgnoreCase);
}

public readonly record struct WorldAuthorityHealth(
    bool Healthy,
    int WorldGeneration,
    WorldManifestIdentity Manifest,
    string ManifestSha256,
    ulong AuthorityContextEpoch,
    ulong LastHostGameplayCommittedSequence,
    ulong LatestAcceptedSequence,
    ulong FailedAuthorityTransaction,
    string FailureReason);

public sealed class WorldReplicationAuthority
{
    private readonly object _gate = new();
    private readonly Guid _sessionId;
    private readonly ulong _hostPeerId;
    private readonly Dictionary<ulong, WorldNetworkContext> _contexts = new();
    private readonly Dictionary<ulong, ulong> _lastIntentSequence = new();
    private readonly Dictionary<StateKey, ulong> _revisions = new();
    private readonly Dictionary<StateKey, WorldNetworkMutation> _replay = new();
    private readonly Dictionary<ulong, RecentTypedMessage> _recentTypedMessages = new();
    private readonly HashSet<(ulong Peer, uint Generation, ulong Cause)> _publicCauses = new();
    private readonly Queue<(ulong Peer, uint Generation, ulong Cause)> _publicCauseOrder = new();
    private const int MaximumRecentPublicCauses = 8192;
    private sealed class PendingSegment(WorldPublicAction first) {
        public WorldPublicAction First { get; } = first;
        public List<WorldNetworkIntent> Roots { get; } = new(first.RootCount);
    }
    private readonly Dictionary<ulong, PendingSegment> _pendingSegments = new();
    private readonly Dictionary<ulong, ulong> _lastSegmentSerial = new();
    private readonly Queue<WorldNetworkMutation[]> _transactionJournal = new();
    private const int MaximumJournalTransactions = 128;
    private WorldNetworkContext? _hostContext;
    private bool _authorityHealthy = true;
    private int _authorityHealthGeneration;
    private WorldManifestIdentity _authorityHealthManifest;
    private string _authorityHealthManifestSha256 = string.Empty;
    private ulong _authorityContextEpoch;
    private ulong _lastHostGameplayCommittedSequence;
    private ulong _failedAuthorityTransaction;
    private string _authorityFailureReason = string.Empty;
    private ulong _nextSequence = 1;
    private ulong _nextTimelineTick = 1;
    private const long TypedCrossPeerDedupWindowMs = 1000;
    private const int MaximumRecentTypedMessages = 2048;

    public WorldReplicationAuthority(Guid sessionId, ulong hostPeerId)
    {
        if (sessionId == Guid.Empty) throw new ArgumentException("SessionId is invalid.", nameof(sessionId));
        if (hostPeerId == 0) throw new ArgumentOutOfRangeException(nameof(hostPeerId));
        _sessionId = sessionId;
        _hostPeerId = hostPeerId;
    }

    private static bool IsLocalFirstClientRoot(
        WorldPublicAction root, WorldEntityId owner)
    {
        if ((root.Flags & WorldPublicActionCodec.AwaitAuthority) != 0)
            return false;

        // Patch 5.12: preserve the exact actor/context metadata of the generic
        // StateTransition carrier observed on the Client. Host-authored working
        // carriers commonly use WorldEntityActor (and EmittedOutput may use
        // AuthorityActor), so forcing SourcePlayer here would no longer be the
        // same semantic action. The Native sender only reaches this path after
        // WM09 certified-state-carrier admission; the Host still authenticates
        // the peer, orders the transaction and broadcasts the authoritative copy.
        if (root.RootKind is WorldPublicActionCodec.CompositeInputRoot
            or WorldPublicActionCodec.EmittedOutputRoot)
        {
            var frames = root.ContextFrames ?? Array.Empty<WorldPublicContextFrame>();
            var validContext = root.ContextKind == WorldPublicActionCodec.NativeContextChain
                ? frames.Length > 0 && root.Flags == WorldPublicActionCodec.DetachedCallback
                : root.ContextKind == WorldPublicActionCodec.AutonomousContext
                  && frames.Length == 0
                  && root.Flags is 0 or WorldPublicActionCodec.ExternalInvocation;
            return root.Target == owner && validContext;
        }

        if (root.ActorKind != WorldPublicActionCodec.SourcePlayerActor)
            return false;

        if (root.RootKind == WorldPublicActionCodec.TypedMessageRoot)
            return true;

        if (root.RootKind == WorldPublicActionCodec.CanonicalCompositeInputRoot)
            return root.Flags == 0 && root.Target == owner;

        if (root.RootKind == WorldPublicActionCodec.DoorLockStateRoot)
            return root.Flags == 0 && root.Target != owner;

        return false;
    }

    public void SetHostContext(WorldNetworkContext context)
    {
        WorldReplicationValidation.Validate(context);
        if (context.SessionId != _sessionId || context.PeerId != _hostPeerId)
            throw new ProtocolException("The host world context identity is invalid.");
        lock (_gate)
        {
            ResetIfWorldChanged(context);
            _hostContext = context;
            _contexts[_hostPeerId] = context;
            if (!context.Ready)
            {
                _lastIntentSequence.Remove(_hostPeerId);
                _pendingSegments.Remove(_hostPeerId);
                _lastSegmentSerial.Remove(_hostPeerId);
            }
        }
    }

    public void SetPeerContext(WorldNetworkContext context)
    {
        WorldReplicationValidation.Validate(context);
        if (context.SessionId != _sessionId || context.PeerId == _hostPeerId)
            throw new ProtocolException("The peer world context identity is invalid.");
        lock (_gate)
        {
            if (_hostContext is null || context.WorldGeneration != _hostContext.WorldGeneration)
                throw new ProtocolException("The peer world context generation is stale.");
            _contexts[context.PeerId] = context;
            // A local LevelGeneration change resets the DLL-side intent sequence.
            // Ready=false is ordered on the same TCP stream before the peer can
            // re-acknowledge this world, so it is the safe epoch boundary for
            // that peer's client sequence even when WorldGeneration is unchanged.
            if (!context.Ready)
            {
                _lastIntentSequence.Remove(context.PeerId);
                _pendingSegments.Remove(context.PeerId);
                _lastSegmentSerial.Remove(context.PeerId);
            }
        }
    }

    public void RemovePeer(ulong peerId)
    {
        lock (_gate)
        {
            _contexts.Remove(peerId);
            _lastIntentSequence.Remove(peerId);
            _pendingSegments.Remove(peerId);
            _lastSegmentSerial.Remove(peerId);
        }
    }

    public bool IsPeerCompatible(ulong peerId)
    {
        lock (_gate)
        {
            return _hostContext is not null
                && _contexts.TryGetValue(peerId, out var peer)
                && WorldReplicationValidation.Compatible(_hostContext, peer);
        }
    }

    public bool IsPeerRecoveryCompatible(ulong peerId, int worldGeneration)
    {
        lock (_gate)
        {
            return _hostContext is { Ready: true }
                && _hostContext.WorldGeneration == worldGeneration
                && _contexts.TryGetValue(peerId, out var peer)
                && WorldReplicationValidation.Compatible(_hostContext, peer);
        }
    }

    public bool AllPeersCompatible(IEnumerable<ulong> peerIds)
    {
        ArgumentNullException.ThrowIfNull(peerIds);
        lock (_gate)
        {
            if (_hostContext is null || !_hostContext.Ready) return false;
            foreach (var peerId in peerIds)
            {
                if (peerId == _hostPeerId) continue;
                if (!_contexts.TryGetValue(peerId, out var peer)
                    || !WorldReplicationValidation.Compatible(_hostContext, peer)) return false;
            }
            return true;
        }
    }

    public WorldAuthorityHealth AuthorityHealth
    {
        get
        {
            lock (_gate) return SnapshotAuthorityHealthLocked();
        }
    }

    public bool TryGetAuthorityContextIdentity(out WorldAuthorityContextIdentity identity)
    {
        lock (_gate)
        {
            if (_hostContext is not { } context || _authorityContextEpoch == 0U)
            {
                identity = default;
                return false;
            }
            identity = SnapshotAuthorityContextIdentityLocked(context);
            return true;
        }
    }

    public bool IsCurrentAuthorityContext(WorldAuthorityContextIdentity expected)
    {
        lock (_gate) return AuthorityContextMatchesLocked(expected);
    }

    public bool CanBroadcast(WorldNetworkMutation mutation)
    {
        ArgumentNullException.ThrowIfNull(mutation);
        lock (_gate)
        {
            return _authorityHealthy
                && _hostContext is { Ready: true } context
                && mutation.SessionId == _sessionId
                && mutation.WorldGeneration == context.WorldGeneration
                && mutation.Mutation.Manifest == context.Manifest;
        }
    }

    public bool TryCommitHostGameplayTransaction(
        WorldAuthorityContextIdentity authorityContext,
        ulong startSequence,
        ulong endSequence,
        out string rejection)
    {
        rejection = string.Empty;
        lock (_gate)
        {
            // Step 4.3: sequence numbers are scoped by the complete authority
            // incarnation. Generation/manifest alone cannot distinguish A->B->A.
            if (!AuthorityContextMatchesLocked(authorityContext))
            {
                rejection = "HOST_GAMEPLAY_COMMIT_CONTEXT_STALE";
                return false;
            }
            if (!_authorityHealthy)
            {
                rejection = "WS3_AUTHORITY_UNHEALTHY";
                return false;
            }
            if (startSequence == 0 || endSequence < startSequence
                || startSequence != _lastHostGameplayCommittedSequence + 1U
                || endSequence >= _nextSequence)
            {
                rejection = "HOST_GAMEPLAY_COMMIT_ORDER_INVALID";
                return false;
            }
            _lastHostGameplayCommittedSequence = endSequence;
            return true;
        }
    }

    public bool TryFailStopAuthority(
        WorldAuthorityContextIdentity authorityContext,
        ulong failedAuthorityTransaction,
        string reason,
        out WorldAuthorityHealth health)
    {
        if (string.IsNullOrWhiteSpace(reason)) reason = "Host gameplay commit failed.";
        lock (_gate)
        {
            // Validate BEFORE mutating health. A late native result from a
            // superseded authority epoch is diagnostic only.
            if (!AuthorityContextMatchesLocked(authorityContext))
            {
                health = SnapshotAuthorityHealthLocked();
                return false;
            }
            if (!_authorityHealthy)
            {
                health = SnapshotAuthorityHealthLocked();
                return false;
            }
            _authorityHealthy = false;
            _failedAuthorityTransaction = failedAuthorityTransaction;
            _authorityFailureReason = reason.Length <= 192 ? reason : reason[..192];
            // Any fragment not yet authority-assigned belongs to a generation
            // whose Host CATHODE state is no longer a valid continuation.
            _pendingSegments.Clear();
            health = SnapshotAuthorityHealthLocked();
            return true;
        }
    }

    private bool AuthorityContextMatchesLocked(WorldAuthorityContextIdentity expected) =>
        _hostContext is { Ready: true } context
        && expected.SessionId == _sessionId
        && expected.SessionId == context.SessionId
        && expected.WorldGeneration == context.WorldGeneration
        && expected.Manifest == context.Manifest
        && string.Equals(expected.ManifestSha256, context.ManifestSha256,
            StringComparison.OrdinalIgnoreCase)
        && expected.Epoch != 0U
        && expected.Epoch == _authorityContextEpoch;

    private WorldAuthorityContextIdentity SnapshotAuthorityContextIdentityLocked(
        WorldNetworkContext context) => new(
        _sessionId,
        context.WorldGeneration,
        context.Manifest,
        context.ManifestSha256,
        _authorityContextEpoch);

    private WorldAuthorityHealth SnapshotAuthorityHealthLocked() => new(
        _authorityHealthy,
        _authorityHealthGeneration,
        _authorityHealthManifest,
        _authorityHealthManifestSha256,
        _authorityContextEpoch,
        _lastHostGameplayCommittedSequence,
        _nextSequence - 1U,
        _failedAuthorityTransaction,
        _authorityFailureReason);

    public bool TryAcceptCausalFragment(WorldNetworkIntent envelope, ulong authenticatedPeerId,
        out IReadOnlyList<WorldNetworkMutation> mutations,
        out IReadOnlyList<WorldNetworkIntent> rejectedIntents, out string rejection)
    {
        mutations = [];
        rejectedIntents = [];
        rejection = string.Empty;
        lock (_gate)
        {
            // Validation is done before a fragment can allocate assembly state.
            try { WorldReplicationValidation.Validate(envelope); }
            catch (ProtocolException exception) {
                rejection = exception.Message; rejectedIntents = [envelope]; return false;
            }
            if (_hostContext is { } current
                && envelope.WorldGeneration == current.WorldGeneration
                && !_authorityHealthy)
            {
                rejection = "WS3_AUTHORITY_UNHEALTHY";
                rejectedIntents = [envelope];
                return false;
            }
            if (envelope.Intent.Operation != WorldReplicationConstants.CompositePublicInputOperation) {
                if (TryAcceptIntent(envelope, authenticatedPeerId, out var single, out rejection)) {
                    // Recovery is sequence-based, so the journal must remain
                    // contiguous even when an authoritative singleton (probe or
                    // replayable state mutation) occurs between WS3 ROOT segments.
                    EnqueueTransactionLocked([single]);
                    mutations = [single]; return true;
                }
                rejectedIntents = [envelope]; return false;
            }
            string? error = null;
            if (_hostContext is null || !_hostContext.Ready || envelope.SessionId != _sessionId ||
                envelope.WorldGeneration != _hostContext.WorldGeneration ||
                authenticatedPeerId == 0 || envelope.Intent.SourcePeer != authenticatedPeerId ||
                !_contexts.TryGetValue(authenticatedPeerId, out var context) ||
                !WorldReplicationValidation.Compatible(_hostContext, context) ||
                envelope.Intent.Manifest != _hostContext.Manifest)
                error = "WS3_TRANSACTION_CONTEXT_MISMATCH";
            if (!WorldPublicActionCodec.TryDecode(envelope.Intent.Payload, out var root) || !root.RetailRoot ||
                root.AuthorityTransaction != 0 || envelope.Intent.RequestedKind != WorldMutationKind.Event)
                error = "WS3_TRANSACTION_V13_REQUIRED";
            if (error is null && authenticatedPeerId != _hostPeerId)
            {
                var localFirstClientEvent =
                    IsLocalFirstClientRoot(root, envelope.Intent.Entity);
                var awaitPlayerProposal =
                    root.RootKind != WorldPublicActionCodec.CanonicalCompositeInputRoot &&
                    root.RootKind != WorldPublicActionCodec.TypedMessageRoot &&
                    root.RootKind != WorldPublicActionCodec.DoorLockStateRoot &&
                    root.ActorKind == WorldPublicActionCodec.SourcePlayerActor &&
                    (root.Flags & WorldPublicActionCodec.AwaitAuthority) != 0;
                if (!localFirstClientEvent && !awaitPlayerProposal)
                    error = "WS3_AUTONOMOUS_HOST_AUTHORITY_REQUIRED";
            }

            _pendingSegments.TryGetValue(authenticatedPeerId, out var pending);
            if (error is null && pending is null) {
                if (root.RootOrdinal != 0 ||
                    (_lastSegmentSerial.TryGetValue(authenticatedPeerId, out var previousSegment) &&
                        root.SegmentSerial <= previousSegment))
                    error = "WS3_TRANSACTION_PREFIX_OR_SERIAL_INVALID";
                else {
                    pending = new PendingSegment(root);
                    _pendingSegments[authenticatedPeerId] = pending;
                }
            }
            if (error is null && pending is not null) {
                var first = pending.First;
                var previousSequence = pending.Roots.Count != 0 ? pending.Roots[^1].Intent.ClientSequence
                    : _lastIntentSequence.GetValueOrDefault(authenticatedPeerId);
                if (root.CausalSerial != first.CausalSerial || root.SegmentSerial != first.SegmentSerial ||
                    root.RootCount != first.RootCount || root.RootOrdinal != pending.Roots.Count ||
                    envelope.Intent.ClientSequence <= previousSequence ||
                    (pending.Roots.Count != 0 &&
                        (envelope.Intent.LevelGeneration != pending.Roots[0].Intent.LevelGeneration ||
                         envelope.WorldGeneration != pending.Roots[0].WorldGeneration)))
                    error = "WS3_TRANSACTION_FRAGMENT_ORDER_INVALID";
                else if (pending.Roots.Any(item =>
                    WorldPublicActionCodec.TryDecode(item.Intent.Payload, out var previous) &&
                    previous.ObservationId == root.ObservationId))
                    error = "DUPLICATE_WS3_CAUSE_IN_TRANSACTION";
            }
            if (error is not null || pending is null) {
                rejectedIntents = pending is null ? [envelope] : [.. pending.Roots, envelope];
                _pendingSegments.Remove(authenticatedPeerId);
                rejection = error ?? "WS3_TRANSACTION_UNAVAILABLE";
                return false;
            }
            pending.Roots.Add(envelope);
            if (pending.Roots.Count < pending.First.RootCount) return true;
            _pendingSegments.Remove(authenticatedPeerId);

            // All members are held under one authority lock. Event ROOTs touch
            // only sequence counters and this peer's last-intent counter; no
            // mutation is published until every member has been accepted.
            var firstSequence = _nextSequence;
            var firstTimeline = _nextTimelineTick;
            var hadPrevious = _lastIntentSequence.TryGetValue(authenticatedPeerId, out var oldSequence);
            var accepted = new List<WorldNetworkMutation>(pending.Roots.Count);
            foreach (var proposal in pending.Roots) {
                var payload = proposal.Intent.Payload.ToArray();
                BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(88, 8), firstSequence);
                var canonical = proposal with { Intent = proposal.Intent with { Payload = payload } };
                if (!TryAcceptIntent(canonical, authenticatedPeerId, out var mutation, out rejection,
                        rememberPublicCause: false)) {
                    _nextSequence = firstSequence;
                    _nextTimelineTick = firstTimeline;
                    if (hadPrevious) _lastIntentSequence[authenticatedPeerId] = oldSequence;
                    else _lastIntentSequence.Remove(authenticatedPeerId);
                    rejectedIntents = pending.Roots.ToArray();
                    return false;
                }
                accepted.Add(mutation);
            }
            foreach (var proposal in pending.Roots) {
                WorldPublicActionCodec.TryDecode(proposal.Intent.Payload, out var member);
                RememberPublicCause((authenticatedPeerId, proposal.Intent.LevelGeneration, member.ObservationId));
            }
            _lastSegmentSerial[authenticatedPeerId] = pending.First.SegmentSerial;
            var transaction = accepted.ToArray();
            EnqueueTransactionLocked(transaction);
            mutations = transaction;
            return true;
        }
    }

    public ulong LatestAuthoritySequence
    {
        get { lock (_gate) return _nextSequence - 1; }
    }

    private void EnqueueTransactionLocked(WorldNetworkMutation[] transaction)
    {
        if (transaction.Length == 0) return;
        _transactionJournal.Enqueue(transaction);
        while (_transactionJournal.Count > MaximumJournalTransactions)
            _transactionJournal.Dequeue();
    }

    // Journal of complete transactions for targeted recovery. A caller must
    // use the receiver's last *completed* sequence, never replay a partially
    // invoked segment. Persistent-state ReplaySnapshot remains separate.
    public bool TryGetCausalTransactionsAfter(ulong completedSequence,
        out IReadOnlyList<WorldNetworkMutation> missing)
    {
        lock (_gate) {
            missing = [];
            // Journaled means HostAccepted, not HostGameplayCommitted. Recovery
            // is allowed only through the Host's last fully committed gameplay
            // transaction; accepted-but-uncommitted entries remain diagnostic
            // journal state and are never exposed as a healthy suffix.
            if (!_authorityHealthy) return false;
            if (_lastHostGameplayCommittedSequence != _nextSequence - 1U)
                return false; // an accepted transaction is still uncommitted
            var selected = _transactionJournal.Where(tx =>
                tx[^1].Mutation.Sequence > completedSequence
                && tx[^1].Mutation.Sequence <= _lastHostGameplayCommittedSequence).ToArray();
            if (selected.Length == 0)
                return completedSequence == _lastHostGameplayCommittedSequence;
            if (selected[0][0].Mutation.Sequence != completedSequence + 1) return false;
            var expected = completedSequence + 1;
            foreach (var transaction in selected) {
                if (transaction[0].Mutation.Sequence != expected) return false;
                expected = transaction[^1].Mutation.Sequence + 1;
            }
            if (expected != _lastHostGameplayCommittedSequence + 1U) return false;
            missing = selected.SelectMany(static tx => tx).ToArray();
            return true;
        }
    }

    public bool TryAcceptIntent(
        WorldNetworkIntent envelope,
        ulong authenticatedPeerId,
        out WorldNetworkMutation mutation,
        out string rejection,
        bool rememberPublicCause = true)
    {
        mutation = default!;
        rejection = string.Empty;
        try { WorldReplicationValidation.Validate(envelope); }
        catch (ProtocolException exception) { rejection = exception.Message; return false; }

        lock (_gate)
        {
            if (_hostContext is null || !_hostContext.Ready)
            {
                rejection = "HOST_WORLD_CONTEXT_UNAVAILABLE"; return false;
            }
            if (envelope.WorldGeneration == _hostContext.WorldGeneration && !_authorityHealthy)
            {
                rejection = "WS3_AUTHORITY_UNHEALTHY"; return false;
            }
            if (envelope.SessionId != _sessionId
                || envelope.WorldGeneration != _hostContext.WorldGeneration)
            {
                rejection = "STALE_SESSION_GENERATION"; return false;
            }
            if (authenticatedPeerId == 0 || envelope.Intent.SourcePeer != authenticatedPeerId)
            {
                rejection = "SOURCE_PEER_MISMATCH"; return false;
            }
            if (!_contexts.TryGetValue(authenticatedPeerId, out var peerContext)
                || !WorldReplicationValidation.Compatible(_hostContext, peerContext))
            {
                rejection = "MANIFEST_INCOMPATIBLE"; return false;
            }
            if (envelope.Intent.Manifest != _hostContext.Manifest)
            {
                rejection = "MANIFEST_IDENTITY_MISMATCH"; return false;
            }
            // v12.7 object-specific operation IDs are dormant compatibility
            // inputs only. Keep their old host-authority rule so an older local
            // producer cannot turn them into a client-authored world mutation.
            if (envelope.Intent.Operation == WorldAnimatedAdapterCodec.AnimatedInputEdgeOperation
                && authenticatedPeerId != _hostPeerId)
            {
                rejection = "HOST_AUTHORITY_REQUIRED_FOR_ANIMATED_WORLD"; return false;
            }
            if ((envelope.Intent.Operation is WorldReplicationConstants.CathodeTriggerInputOperation
                    or WorldReplicationConstants.ObservedCathodeInputEdgeOperation
                    or WorldReplicationConstants.ObservedCathodeCallbackInputEdgeOperation
                    or WorldReplicationConstants.ObservedCathodeCharacterCallbackInputEdgeOperation
                    or WorldReplicationConstants.ObservedCathodeTypedMessageOperation
                    or WorldReplicationConstants.CompositePublicInputOperation)
                && envelope.Intent.RequestedKind != WorldMutationKind.Event)
            {
                rejection = "CATHODE_ONE_SHOT_REQUIRES_EVENT"; return false;
            }
            if (envelope.Intent.Operation == WorldReplicationConstants.ObservedCathodeLogicSwitchStateOperation
                && envelope.Intent.RequestedKind != WorldMutationKind.State)
            {
                rejection = "CATHODE_LOGIC_SWITCH_REQUIRES_STATE"; return false;
            }
            var isPublicRoot = envelope.Intent.Operation == WorldReplicationConstants.CompositePublicInputOperation;
            WorldPublicAction publicAction = default;
            if (isPublicRoot && !WorldPublicActionCodec.TryDecode(envelope.Intent.Payload, out publicAction))
            {
                rejection = "WS3_CONTEXT_PROTOCOL_V13_REQUIRED"; return false;
            }
            var causeKey = (authenticatedPeerId, envelope.Intent.LevelGeneration, publicAction.ObservationId);
            if (isPublicRoot && publicAction.RetailRoot)
            {
                // Personal interactions carry the authenticated source player.
                // Autonomous/AI decisions are authored only by the host.
                if (authenticatedPeerId != _hostPeerId)
                {
                    var localFirstClientEvent =
                        IsLocalFirstClientRoot(publicAction, envelope.Intent.Entity);
                    var awaitPlayerProposal =
                        publicAction.RootKind != WorldPublicActionCodec.CanonicalCompositeInputRoot &&
                        publicAction.RootKind != WorldPublicActionCodec.TypedMessageRoot &&
                        publicAction.RootKind != WorldPublicActionCodec.DoorLockStateRoot &&
                        publicAction.ActorKind == WorldPublicActionCodec.SourcePlayerActor &&
                        (publicAction.Flags & WorldPublicActionCodec.AwaitAuthority) != 0;
                    if (!localFirstClientEvent && !awaitPlayerProposal)
                    {
                        rejection = "WS3_AUTONOMOUS_HOST_AUTHORITY_REQUIRED"; return false;
                    }
                }
                // Step 3: SourcePlayer is authenticated causal attribution, not
                // peer authority. A certified Player-origin World boundary may use
                // AwaitAuthority: the Client has submitted the cause but deliberately
                // has not executed that World decision, so the sequenced mutation must
                // be broadcast back to and applied by the initiating Client. Autonomous
                // non-Player ROOTs remain Host-only.
                if (_publicCauses.Contains(causeKey))
                {
                    rejection = "DUPLICATE_WS3_CAUSE"; return false;
                }
            }
            if (_lastIntentSequence.TryGetValue(authenticatedPeerId, out var previous)
                && envelope.Intent.ClientSequence <= previous)
            {
                rejection = envelope.Intent.ClientSequence == previous
                    ? "DUPLICATE_INTENT" : "STALE_INTENT";
                return false;
            }
            // Generic CATHODE typed-message/blob records are valid WorldIntent
            // proposals from any authenticated compatible peer. The host remains
            // the sole authority that assigns global sequence/timeline ordering.
            // Equivalent deterministic messages observed on two different peers
            // within a short window are collapsed by content so downstream CATHODE
            // consequences are not executed twice. Sequence validation happens
            // first so rejected stale intents cannot poison the de-duplication cache.
            if (envelope.Intent.Operation == WorldReplicationConstants.ObservedCathodeTypedMessageOperation
                && IsCrossPeerTypedDuplicate(envelope.Intent, authenticatedPeerId))
            {
                rejection = "DUPLICATE_CATHODE_MESSAGE"; return false;
            }

            var kind = envelope.Intent.RequestedKind;
            var key = new StateKey(envelope.Intent.Entity, envelope.Intent.Operation, kind);
            var revision = 0UL;
            var flags = WorldMutationFlags.Reliable;
            if (kind is WorldMutationKind.State or WorldMutationKind.Timeline)
            {
                revision = _revisions.TryGetValue(key, out var current) ? current + 1 : 1;
                _revisions[key] = revision;
                flags |= WorldMutationFlags.Replaceable | WorldMutationFlags.Replayable;
            }
            if (envelope.Intent.Operation == WorldReplicationConstants.NetworkProbeOperation)
                flags |= WorldMutationFlags.SyntheticProbe;

            var sequence = _nextSequence++;
            var timeline = _nextTimelineTick++;
            var authoritative = new WorldMutation(
                _hostContext.Manifest,
                kind,
                flags,
                envelope.Intent.Entity,
                checked((uint)_hostContext.WorldGeneration),
                sequence,
                revision,
                timeline,
                authenticatedPeerId,
                envelope.Intent.Operation,
                envelope.Intent.Payload.ToArray());
            mutation = new WorldNetworkMutation(
                _sessionId,
                _hostContext.WorldGeneration,
                authoritative);
            _lastIntentSequence[authenticatedPeerId] = envelope.Intent.ClientSequence;
            if (isPublicRoot && publicAction.RetailRoot && rememberPublicCause) RememberPublicCause(causeKey);
            if ((flags & WorldMutationFlags.Replayable) != 0)
            {
                if (_replay.Count >= WorldReplicationConstants.MaximumReplayStates && !_replay.ContainsKey(key))
                {
                    var oldest = _replay.OrderBy(pair => pair.Value.Mutation.Sequence).First();
                    _replay.Remove(oldest.Key);
                }
                _replay[key] = mutation;
            }
            return true;
        }
    }

    public IReadOnlyList<WorldNetworkMutation> ReplaySnapshot()
    {
        lock (_gate)
            return _replay.Values.OrderBy(value => value.Mutation.Sequence).ToArray();
    }

    private void ResetIfWorldChanged(WorldNetworkContext context)
    {
        var changed = _hostContext is null
            || _hostContext.WorldGeneration != context.WorldGeneration
            || _hostContext.Manifest != context.Manifest
            || !string.Equals(_hostContext.ManifestSha256, context.ManifestSha256,
                StringComparison.OrdinalIgnoreCase);
        if (!changed) return;
        if (_hostContext is not null)
        {
            _contexts.Clear();
            _lastIntentSequence.Clear();
            _revisions.Clear();
            _replay.Clear();
            _recentTypedMessages.Clear();
            _publicCauses.Clear();
            _publicCauseOrder.Clear();
            _pendingSegments.Clear();
            _lastSegmentSerial.Clear();
            _transactionJournal.Clear();
            _nextSequence = 1;
            _nextTimelineTick = 1;
        }
        _authorityHealthy = true;
        _authorityHealthGeneration = context.WorldGeneration;
        _authorityHealthManifest = context.Manifest;
        _authorityHealthManifestSha256 = context.ManifestSha256;
        _authorityContextEpoch = unchecked(_authorityContextEpoch + 1U);
        if (_authorityContextEpoch == 0U) _authorityContextEpoch = 1U;
        _lastHostGameplayCommittedSequence = 0;
        _failedAuthorityTransaction = 0;
        _authorityFailureReason = string.Empty;
    }

    private void RememberPublicCause((ulong Peer, uint Generation, ulong Cause) cause)
    {
        if (!_publicCauses.Add(cause)) return;
        _publicCauseOrder.Enqueue(cause);
        while (_publicCauseOrder.Count > MaximumRecentPublicCauses)
            _publicCauses.Remove(_publicCauseOrder.Dequeue());
    }


    private bool IsCrossPeerTypedDuplicate(WorldIntent intent, ulong peerId)
    {
        var typed = WorldCathodeBusCodec.DecodeTypedMessage(intent.Payload);
        var fingerprint = TypedMessageFingerprint(intent.Entity, typed);
        var now = Environment.TickCount64;

        if (_recentTypedMessages.TryGetValue(fingerprint, out var recent)
            && recent.ExpiresAtMs > now && recent.PeerId != peerId)
            return true;

        if (_recentTypedMessages.Count >= MaximumRecentTypedMessages)
        {
            foreach (var stale in _recentTypedMessages
                .Where(pair => pair.Value.ExpiresAtMs <= now)
                .Select(pair => pair.Key)
                .ToArray())
                _recentTypedMessages.Remove(stale);
            if (_recentTypedMessages.Count >= MaximumRecentTypedMessages)
                _recentTypedMessages.Clear();
        }

        _recentTypedMessages[fingerprint] = new RecentTypedMessage(
            peerId, now + TypedCrossPeerDedupWindowMs);
        return false;
    }

    private static ulong TypedMessageFingerprint(
        WorldEntityId target,
        WorldCathodeTypedMessagePayload message)
    {
        const ulong offset = 1469598103934665603UL;
        const ulong prime = 1099511628211UL;
        var hash = offset;

        void MixByte(ref ulong value, byte item)
        {
            value ^= item;
            value *= prime;
        }

        void MixUInt64(ref ulong value, ulong item)
        {
            for (var shift = 0; shift < 64; shift += 8)
                MixByte(ref value, (byte)(item >> shift));
        }

        MixUInt64(ref hash, message.MessageType);
        MixUInt64(ref hash, message.SourceRuntimeKey.EntityA);
        MixUInt64(ref hash, message.SourceRuntimeKey.EntityB);
        MixUInt64(ref hash, message.TargetRuntimeKey.EntityA);
        MixUInt64(ref hash, message.TargetRuntimeKey.EntityB);
        MixUInt64(ref hash, target.High);
        MixUInt64(ref hash, target.Low);
        MixUInt64(ref hash, BitConverter.DoubleToUInt64Bits(message.RelativeDelaySeconds));
        MixUInt64(ref hash, checked((ulong)message.Data.Length));
        foreach (var item in message.Data) MixByte(ref hash, item);
        return hash == 0 ? 1UL : hash;
    }

    private readonly record struct StateKey(
        WorldEntityId Entity,
        uint Operation,
        WorldMutationKind Kind);

    private readonly record struct RecentTypedMessage(ulong PeerId, long ExpiresAtMs);
}
