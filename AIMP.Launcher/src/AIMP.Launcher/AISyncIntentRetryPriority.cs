using AIMP.Protocol;

namespace AIMP.Launcher;

// Only retry hints are coalesced. Original AIEX records and native admission
// remain untouched. Store small ordering keys, never copies of movement bundles.
internal static class AISyncIntentRetryPriority
{
    internal readonly record struct Candidate(ulong Sequence, ulong Command, uint Kind,
        AISyncTransactionOpcode Opcode, uint Admission, uint Revision)
    {
        internal static Candidate From(in AISyncTransaction operation) => new(
            operation.GlobalSequence, operation.CommandId, operation.NativeKind,
            operation.Opcode, operation.NativeAdmission, operation.ParameterRevision);
        internal bool Scalar => Replaceable(Kind) && IsIntent(Opcode);
        internal bool Update => Replaceable(Kind) && Opcode == AISyncTransactionOpcode.Update;
        internal bool Bypass => Opcode == AISyncTransactionOpcode.Submit &&
            (Kind is >= 0x100 and <= 0x105 or 0x200);
        internal bool MayPrecedeBypass => IsIntent(Opcode) && (Replaceable(Kind) || Kind == 5);
    }

    internal struct Selection
    {
        internal Candidate First, Latest;
        private Candidate _bypass;
        private ulong _barrier;
        private uint _maxRevision;
        private int _maxRevisionCount;
        private bool _allScalar, _sameUpdates;

        internal void Add(in AISyncTransaction operation)
        {
            var candidate = Candidate.From(operation);
            if (First.Sequence == 0) Initialize(candidate);
            _allScalar &= candidate.Scalar;
            _sameUpdates &= candidate.Update && candidate.Command == First.Command && candidate.Kind == First.Kind;
            if (candidate.Sequence < First.Sequence) First = candidate;
            if (candidate.Sequence > Latest.Sequence) Latest = candidate;
            ObserveRevision(candidate.Revision);
            ObserveBarrier(candidate);
        }

        private void Initialize(Candidate candidate)
        {
            First = Latest = candidate;
            _allScalar = _sameUpdates = true;
        }

        private void ObserveRevision(uint revision)
        {
            if (revision > _maxRevision) { _maxRevision = revision; _maxRevisionCount = 1; }
            else if (revision == _maxRevision) ++_maxRevisionCount;
        }

        private void ObserveBarrier(Candidate candidate)
        {
            if (!candidate.MayPrecedeBypass && (_barrier == 0 || candidate.Sequence < _barrier))
                _barrier = candidate.Sequence;
            if (candidate.Bypass && (_bypass.Sequence == 0 || candidate.Sequence < _bypass.Sequence))
                _bypass = candidate;
        }

        internal readonly ulong Primary()
        {
            if (Latest.Opcode == AISyncTransactionOpcode.Submit && Latest.Admission == 0 && _allScalar)
                return Latest.Sequence;
            if (_sameUpdates && Latest.Revision == _maxRevision && _maxRevisionCount == 1)
                return Latest.Sequence;
            return First.Sequence;
        }

        // Mirrors only the explicit cross-command privilege in NativeReadiness.
        // Markers, placement, cleanup, controls and earlier constructors block it.
        internal readonly ulong Bypass() => _bypass.Sequence > First.Sequence &&
            _bypass.Command != First.Command && _barrier == _bypass.Sequence ? _bypass.Sequence : 0;
    }

    internal static ulong Select(IEnumerable<AISyncTransaction> pending)
    {
        var selection = new Selection();
        foreach (var operation in pending) selection.Add(operation);
        return selection.Primary();
    }

    private static bool IsIntent(AISyncTransactionOpcode opcode) =>
        opcode is AISyncTransactionOpcode.Submit or AISyncTransactionOpcode.Update;
    private static bool Replaceable(uint kind) => kind is 1 or 3 or 4 or 7 or 9;
}
