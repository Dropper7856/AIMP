using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed class LocalPlayerTorchReader
{
    // Sample both the retail flag and the light's emitted state; an inventory
    // request alone does not prove the beam survived recharge/cutscene gates.
    internal const int EnabledOffset = 0x2BA;
    private readonly Func<nint, byte[], bool> _read;
    internal const int EquipmentOffset = 0x1100, EmittedOffset = 0x1574, LightOffset = 0x156C;
    private readonly byte[] _byte = new byte[1], _pointer = new byte[4];

    internal LocalPlayerTorchReader(Func<nint, byte[], bool> read) => _read = read;

    internal bool TryCapture(nint character, out PlayerTorchSnapshot? state)
    {
        state = null;
        if (character < 0x10000 || !TrySample(character, out var first)
            || !TrySample(character, out var second) || first != second) return false;
        state = new PlayerTorchSnapshot(first.Enabled);
        return true;
    }

    private bool TrySample(nint character, out (uint Equipment, uint Light, bool Enabled) sample)
    {
        sample = default;
        if (!_read(character + EnabledOffset, _byte)) return false;
        var requested = _byte[0] != 0;
        if (!_read(character + EquipmentOffset, _pointer)) return false;
        var equipment = BitConverter.ToUInt32(_pointer);
        if (equipment < 0x10000 || !_read((nint)(long)equipment + LightOffset, _pointer)) return false;
        var light = BitConverter.ToUInt32(_pointer);
        if (!_read((nint)(long)equipment + EmittedOffset, _byte)) return false;
        sample = (equipment, light, requested && light != 0 && _byte[0] != 0);
        return true;
    }
}
