using System.Runtime.InteropServices;
using AIMP.Launcher.Contracts;

namespace AIMP.Launcher;

internal static class VoiceMicrophoneDevices
{
    internal static IReadOnlyList<VoiceMicrophoneDeviceDto> Enumerate()
    {
        var devices = new List<VoiceMicrophoneDeviceDto>
        {
            new("default", "Default communications microphone", true)
        };
        for (uint i = 0; i < WinMm.waveInGetNumDevs(); i++)
        {
            WinMm.Check(WinMm.waveInGetDevCapsW((nuint)i, out var caps, (uint)Marshal.SizeOf<WinMm.Caps>()), "Read microphone information");
            var id = InterfaceId(i);
            // Devices without a persistent interface cannot safely be selected by ordinal.
            if (id is not null && !devices.Any(d => d.Id.Equals(id, StringComparison.OrdinalIgnoreCase)))
                devices.Add(new(id, caps.Name, false));
        }
        return devices;
    }

    internal static uint Resolve(string id)
    {
        if (id == "default") return uint.MaxValue;
        for (uint i = 0; i < WinMm.waveInGetNumDevs(); i++)
            if (string.Equals(InterfaceId(i), id, StringComparison.OrdinalIgnoreCase)) return i;
        throw new InvalidOperationException("The selected microphone is unavailable. Reconnect it or choose another microphone.");
    }

    private static string? InterfaceId(uint index)
    {
        var sizePointer = Marshal.AllocHGlobal(4);
        try
        {
            if (WinMm.waveInMessage((nint)index, 0x80D, sizePointer, 0) != 0) return null;
            var size = Marshal.ReadInt32(sizePointer);
            if (size is < 2 or > 65536) return null;
            var buffer = Marshal.AllocHGlobal(size);
            try
            {
                if (WinMm.waveInMessage((nint)index, 0x80C, buffer, (nuint)size) != 0) return null;
                return Marshal.PtrToStringUni(buffer) is { Length: > 0 } path ? path : null;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        finally { Marshal.FreeHGlobal(sizePointer); }
    }
}

internal static class WinMm
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct Caps
    {
        internal ushort Manufacturer, Product;
        internal uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] internal string Name;
        internal uint Formats;
        internal ushort Channels, Reserved;
    }
    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    internal struct Format
    {
        internal ushort Tag, Channels;
        internal uint SamplesPerSecond, BytesPerSecond;
        internal ushort BlockAlign, BitsPerSample, ExtraSize;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Header
    {
        internal nint Data;
        internal uint BufferLength, BytesRecorded;
        internal nuint User;
        internal uint Flags, Loops;
        internal nint Next;
        internal nuint Reserved;
    }
    internal delegate void Callback(nint handle, uint message, nuint instance, nint header, nuint reserved);
    [DllImport("winmm.dll")] internal static extern uint waveInGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] internal static extern uint waveInGetDevCapsW(nuint id, out Caps caps, uint size);
    [DllImport("winmm.dll")] internal static extern uint waveInMessage(nint id, uint message, nint param1, nuint param2);
    [DllImport("winmm.dll")] internal static extern uint waveInOpen(out nint handle, uint device, ref Format format, Callback callback, nuint instance, uint flags);
    [DllImport("winmm.dll")] internal static extern uint waveInPrepareHeader(nint handle, nint header, uint size);
    [DllImport("winmm.dll")] internal static extern uint waveInUnprepareHeader(nint handle, nint header, uint size);
    [DllImport("winmm.dll")] internal static extern uint waveInAddBuffer(nint handle, nint header, uint size);
    [DllImport("winmm.dll")] internal static extern uint waveInStart(nint handle);
    [DllImport("winmm.dll")] internal static extern uint waveInReset(nint handle);
    [DllImport("winmm.dll")] internal static extern uint waveInClose(nint handle);
    internal static void Check(uint result, string operation)
    {
        if (result != 0) throw new InvalidOperationException($"{operation} failed (WinMM {result}). Check microphone permissions and device availability.");
    }
}
