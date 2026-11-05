using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed class LocalPlayerHidingReader
{
    // FUN_00785B10: NPC_set_behaviour_tree_flags stores one byte per flag at
    // Character+423C. Hiding_logic binds flag 13 PLAYER_HIDING to THE_PLAYER;
    // it rises after the entry animation and clears after the exit/cleanup.
    internal const int PlayerHidingFlagOffset = 0x423C + 13;
    // FUN_006C4510 (NPC_SetHidingNearestLocation): the AI search point of the
    // accepted hiding spot, written at entry start and never cleared.
    internal const int HidingPositionRva = 0x12B18A8;
    private readonly Func<nint, byte[], bool> _read;
    private readonly byte[] _flag = new byte[1], _position = new byte[12];
    private PlayerHidingSnapshot? _latched;
    private nint _character;
    private int _generation;

    internal LocalPlayerHidingReader(Func<nint, byte[], bool> read) => _read = read;

    internal bool TryCapture(nint moduleBase, nint character, int generation,
        out PlayerHidingSnapshot? state, Func<bool>? identityStillCurrent = null)
    {
        state = null;
        if (_character != character || _generation != generation) Reset();
        _character = character;
        _generation = generation;
        if (character < 0x10000 || moduleBase < 0x10000
            || !_read(character + PlayerHidingFlagOffset, _flag)) return false;
        var hidden = _flag[0] != 0;
        PlayerHidingSnapshot? candidate = _latched;
        if (hidden && candidate is null)
        {
            if (!_read(moduleBase + HidingPositionRva, _position)) return false;
            candidate = new PlayerHidingSnapshot(true, BitConverter.ToSingle(_position, 0),
                BitConverter.ToSingle(_position, 4), BitConverter.ToSingle(_position, 8));
            if (!PlayerHidingCodec.IsValid(candidate)) return false;
        }
        // Do not publish a torn entry/exit or latch a spot from a replaced actor.
        if (!_read(character + PlayerHidingFlagOffset, _flag)
            || (_flag[0] != 0) != hidden || identityStillCurrent?.Invoke() == false) return false;
        _latched = hidden ? candidate : null;
        state = hidden ? _latched : new PlayerHidingSnapshot(false);
        return true;
    }

    internal void Reset() { _latched = null; _character = 0; _generation = 0; }
}
