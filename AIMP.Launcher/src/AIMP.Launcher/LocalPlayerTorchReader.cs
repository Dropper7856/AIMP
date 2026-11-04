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
    private readonly byte[] _rotation = new byte[16];
    // The exact active/fallback camera pair used by retail FUN_007ABFB0.
    internal const int EngineGlobalRva = 0x12F0C88, CameraManagerOffset = 0x58;
    internal const int ActiveCameraOffset = 0x1D8, FallbackCameraOffset = 0x1F0;
    internal const int CameraRotationOffset = 0x2C;

    internal LocalPlayerTorchReader(Func<nint, byte[], bool> read) => _read = read;

    internal bool TryCapture(nint character, out PlayerTorchSnapshot? state, nint moduleBase = 0)
    {
        state = null;
        if (character < 0x10000 || !TrySample(character, out var first)) return false;
        var captured = new PlayerTorchSnapshot(first.Enabled);
        if (first.Enabled && TryReadAim(moduleBase, out var x, out var y, out var z))
            captured = new PlayerTorchSnapshot(true, x, y, z);
        // Reject equipment swaps and on/off edges torn across the camera read.
        if (!TrySample(character, out var second) || first != second) return false;
        state = captured;
        return true;
    }

    private bool TryPointer(nint address, out nint pointer)
    {
        pointer = 0;
        if (!_read(address, _pointer)) return false;
        pointer = (nint)(long)BitConverter.ToUInt32(_pointer);
        return pointer >= 0x10000;
    }

    private bool TryCamera(nint moduleBase, out nint camera)
    {
        camera = 0;
        return moduleBase >= 0x10000
            && TryPointer(moduleBase + EngineGlobalRva, out var engine)
            && TryPointer(engine + CameraManagerOffset, out var manager)
            && (TryPointer(manager + ActiveCameraOffset, out camera)
                || TryPointer(manager + FallbackCameraOffset, out camera));
    }

    private bool TryReadAim(nint moduleBase, out float x, out float y, out float z)
    {
        x = y = z = 0;
        if (!TryCamera(moduleBase, out var camera) || !_read(camera + CameraRotationOffset, _rotation)) return false;
        var qx = BitConverter.ToSingle(_rotation, 0);
        var qy = BitConverter.ToSingle(_rotation, 4);
        var qz = BitConverter.ToSingle(_rotation, 8);
        var qw = BitConverter.ToSingle(_rotation, 12);
        var length = qx * qx + qy * qy + qz * qz + qw * qw;
        if (!float.IsFinite(length) || length is < 0.99F or > 1.01F) return false;
        // FUN_0042A0E0(camera) - FUN_004295B0(camera): camera's +Z ray.
        x = 2 * (qw * qy + qz * qx);
        y = 2 * (qy * qz - qw * qx);
        z = 1 - 2 * (qx * qx + qy * qy);
        var inverseLength = 1 / MathF.Sqrt(x * x + y * y + z * z);
        x *= inverseLength; y *= inverseLength; z *= inverseLength;
        return float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z)
            && TryCamera(moduleBase, out var after) && camera == after;
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
