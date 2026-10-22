using AIMP.Protocol;

namespace AIMP.Launcher;

internal readonly record struct PlayerTraversalKey(ushort Index, byte Type, uint Owner);
internal readonly record struct LocalPostureSample(
    bool Crouched, bool DeepCrouched, bool InVent, uint VentA, uint VentB,
    nint PhysicalCycle, byte State, ushort Link, nint Root, nint Navigation, nint Records,
    PlayerTraversalKey Traversal, uint FlagsLow = 0, ushort FlagsHigh = 0, uint RecordCount = 0);

internal static class LocalPlayerPostureMemoryLayout
{
    internal const int FlagsOffset = 0x2AF, FlagsSize = 6, VentPairOffset = 0x4290;
    internal const int PhysicalCycleOffset = 0x36C, StateOffset = 0x1A, LinkOffset = 0x42;
    internal const int CycleOwnerOffset = 4;
    internal const byte TraversalState = 0x16;
    internal const int NavigationRootOffset = 0x12F0C88, NavigationOffset = 0x20;
    internal const int TraversalArrayOffset = 0x23EC, TraversalStride = 0x10C;
    // FUN_00739460 stores the array count at this+4 (nav+23F0).
    internal const int TraversalCountOffset = 0x23F0;

    // AI.exe.c FUN_00736a80: *(nav+23EC) + (oneBasedIndex-1)*10C.
    // The array is a pointer, not an inline object or another pointer wrapper.
    internal static bool TryResolveTraversal(nint records, ushort index, out nint address)
    {
        var candidate = (long)records + ((long)index - 1) * TraversalStride;
        address = 0;
        if (records < 0x10000 || index == 0 || candidate > uint.MaxValue - 8L) return false;
        address = (nint)candidate;
        return true;
    }

    internal static bool TryDecodeTraversal(ReadOnlySpan<byte> bytes, ushort link, out PlayerTraversalKey key)
    {
        key = default;
        if (bytes.Length < 8 || link == 0 || BitConverter.ToUInt16(bytes[..2]) != link) return false;
        var type = bytes[2];
        if (!PlayerPostureCodec.IsVentTraversal(type)) return true;
        var owner = BitConverter.ToUInt32(bytes.Slice(4, 4));
        if (owner == uint.MaxValue) return false;
        key = new(link, type, owner);
        return true;
    }
}

internal sealed class LocalPlayerPostureHistory
{
    private ulong _revision;
    private PlayerTraversalKey _lastKey;
    private bool _active;

    internal PlayerPostureSnapshot Observe(LocalPostureSample sample)
    {
        var active = sample.State == LocalPlayerPostureMemoryLayout.TraversalState && sample.Traversal.Index != 0;
        if (active && (!_active || sample.Traversal != _lastKey))
        {
            _revision = checked(_revision + 1);
            _lastKey = sample.Traversal;
        }
        _active = active;
        return new(sample.Crouched, sample.DeepCrouched, sample.InVent, sample.VentA, sample.VentB,
            _lastKey.Index == 0 ? 0 : _revision, _lastKey.Index, _lastKey.Type, _lastKey.Owner, active);
    }

    // Keep the counter across Character replacement so a live network identity
    // never reuses a revision. Clear the key across all lifecycle boundaries.
    internal void Reset() { _lastKey = default; _active = false; }
}

internal sealed class LocalPlayerPostureReader
{
    private readonly Func<nint, byte[], bool> _read;
    private readonly byte[] _flags = new byte[6], _vents = new byte[8], _pointer = new byte[4];
    private readonly byte[] _state = new byte[1], _link = new byte[2], _record = new byte[8];
    private readonly LocalPlayerPostureHistory _history = new();
    private nint _character;
    private int _generation;

    internal LocalPlayerPostureReader(Func<nint, byte[], bool> read) => _read = read;

    internal bool TryCapture(nint moduleBase, nint character, int generation, out PlayerPostureSnapshot? posture,
        Func<bool>? identityStillCurrent = null)
    {
        posture = null;
        if (_character != character || _generation != generation) Reset();
        _character = character;
        _generation = generation;
        if (!TrySample(moduleBase, character, out var first)
            || !TrySample(moduleBase, character, out var second) || first != second
            || identityStillCurrent?.Invoke() == false) return false;
        posture = _history.Observe(first);
        return true;
    }

    internal void Reset() { _character = 0; _generation = 0; _history.Reset(); }

    private bool TrySample(nint moduleBase, nint character, out LocalPostureSample sample)
    {
        sample = default;
        if (!_read(character + LocalPlayerPostureMemoryLayout.FlagsOffset, _flags)
            || !_read(character + LocalPlayerPostureMemoryLayout.VentPairOffset, _vents)
            || !TryPointer(character + LocalPlayerPostureMemoryLayout.PhysicalCycleOffset, out var cycle)
            || !TryPointer(cycle + LocalPlayerPostureMemoryLayout.CycleOwnerOffset, out var owner) || owner != character
            || !_read(cycle + LocalPlayerPostureMemoryLayout.StateOffset, _state)
            || !_read(cycle + LocalPlayerPostureMemoryLayout.LinkOffset, _link)) return false;
        var state = _state[0];
        var link = BitConverter.ToUInt16(_link);
        var basis = new LocalPostureSample(_flags[0] != 0, _flags[1] != 0, _flags[5] != 0,
            BitConverter.ToUInt32(_vents, 0), BitConverter.ToUInt32(_vents, 4), cycle, state, link, 0, 0, 0, default,
            BitConverter.ToUInt32(_flags, 0), BitConverter.ToUInt16(_flags, 4));
        if (!ValidFlagsAndVents(basis)) return false;
        return TryTraversal(moduleBase, basis, out sample);
    }

    private static bool ValidFlagsAndVents(LocalPostureSample value) =>
        ((value.VentA == 0) == (value.VentB == 0)) && (!value.InVent || value.VentA != 0);

    private bool TryTraversal(nint moduleBase, LocalPostureSample basis, out LocalPostureSample sample)
    {
        sample = basis;
        if (basis.State != LocalPlayerPostureMemoryLayout.TraversalState) return true;
        if (!TryPointer(moduleBase + LocalPlayerPostureMemoryLayout.NavigationRootOffset, out var root)
            || !TryPointer(root + LocalPlayerPostureMemoryLayout.NavigationOffset, out var nav)
            || !TryPointer(nav + LocalPlayerPostureMemoryLayout.TraversalArrayOffset, out var records)
            || !_read(nav + LocalPlayerPostureMemoryLayout.TraversalCountOffset, _pointer)) return false;
        var count = BitConverter.ToUInt32(_pointer);
        if (count > ushort.MaxValue || basis.Link > count
            || !LocalPlayerPostureMemoryLayout.TryResolveTraversal(records, basis.Link, out var address)
            || !_read(address, _record)
            || !LocalPlayerPostureMemoryLayout.TryDecodeTraversal(_record, basis.Link, out var key)) return false;
        sample = basis with { Root = root, Navigation = nav, Records = records, Traversal = key, RecordCount = count };
        return true;
    }

    private bool TryPointer(nint address, out nint pointer)
    {
        pointer = 0;
        return _read(address, _pointer)
            && LocalPlayerMovementMemoryLayout.TryDecodePointer(_pointer, out pointer);
    }
}
