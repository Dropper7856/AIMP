using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace AIMP.Launcher;

// Several launchers may run side by side (split-screen: one launcher per game).
// The desktop shell assigns each one a slot (AIMP_INSTANCE, 1 = default); each
// slot has its own settings, API port and UI bridge ports. The registry lets
// launchers see which AI.exe belongs to which slot.
internal static class LauncherInstances
{
    internal const int MaximumSlots = 4;
    internal const string SlotEnvironmentVariable = "AIMP_INSTANCE";
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private static readonly object Gate = new();
    private static int _gameProcessId;
    private static bool _registered;

    internal sealed record Entry(int Slot, int LauncherProcessId, long LauncherStartUtcTicks, int GameProcessId);

    internal static int Slot { get; } = ReadSlot(Environment.GetEnvironmentVariable(SlotEnvironmentVariable));

    internal static bool IsPrimary => Slot == 1;

    // Must match the desktop shell (AIMP.Electron/main.js).
    internal static int ApiPort(int slot) => 5127 + slot - 1;

    // The game's movies connect to the public bridge ports (12345/12346); the
    // launcher that holds them forwards other slots' games to these ports.
    internal static int PrivateCommandPort(int slot) => 12400 + 2 * slot;
    internal static int PrivateEventPort(int slot) => 12401 + 2 * slot;

    internal static int GameProcessId => Volatile.Read(ref _gameProcessId);

    internal static int ReadSlot(string? value) =>
        int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var slot)
        && slot is >= 1 and <= MaximumSlots ? slot : 1;

    // "launcher-settings.json" for slot 1, "launcher-settings.instance-2.json" for slot 2...
    internal static string SlotFileName(string fileName, int slot) => slot == 1 ? fileName
        : Path.GetFileNameWithoutExtension(fileName) + $".instance-{slot}" + Path.GetExtension(fileName);

    internal static void Register()
    {
        lock (Gate)
        {
            _registered = true;
            Write();
        }
    }

    internal static void Unregister()
    {
        lock (Gate)
        {
            _registered = false;
            try { File.Delete(EntryPath(Slot)); }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { }
        }
    }

    // The AI.exe this launcher started or injected.
    internal static void ClaimGame(int processId)
    {
        lock (Gate)
        {
            if (_gameProcessId == processId) return;
            _gameProcessId = processId;
            if (_registered) Write();
        }
    }

    internal static IReadOnlyList<Entry> Others() =>
        Enumerable.Range(1, MaximumSlots).Where(slot => slot != Slot)
            .Select(TryRead).OfType<Entry>().Where(IsAlive).ToArray();

    internal static bool OthersRunning() => Others().Count > 0;

    internal static HashSet<int> GamesClaimedByOthers() =>
        Others().Where(entry => entry.GameProcessId != 0).Select(entry => entry.GameProcessId).ToHashSet();

    // Null: this launcher handles the connection itself. Otherwise the slot
    // whose game opened it.
    internal static int? ForwardSlotFor(IPEndPoint? remote, int listenerPort)
    {
        if (remote is null) return null;
        var owner = TcpConnectionOwner.Find(remote, listenerPort);
        if (owner is null || owner == GameProcessId) return null;
        return Others().FirstOrDefault(entry => entry.GameProcessId == owner)?.Slot;
    }

    private static void Write()
    {
        try
        {
            using var self = Process.GetCurrentProcess();
            var entry = new Entry(Slot, self.Id, self.StartTime.ToUniversalTime().Ticks, _gameProcessId);
            var path = EntryPath(Slot);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(entry, JsonOptions));
            File.Move(path + ".tmp", path, true);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { }
    }

    private static Entry? TryRead(int slot)
    {
        try
        {
            var path = EntryPath(slot);
            return File.Exists(path) ? JsonSerializer.Deserialize<Entry>(File.ReadAllText(path)) : null;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    // A crashed launcher leaves its file behind; its PID may since be reused.
    private static bool IsAlive(Entry entry)
    {
        try
        {
            using var process = Process.GetProcessById(entry.LauncherProcessId);
            return !process.HasExited && process.StartTime.ToUniversalTime().Ticks == entry.LauncherStartUtcTicks;
        }
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException
            or System.ComponentModel.Win32Exception)
        {
            return false;
        }
    }

    private static string EntryPath(int slot) => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AIMP", "Instances", $"slot-{slot}.json");
}

// Which process owns the client side of a loopback TCP connection.
internal static class TcpConnectionOwner
{
    private const int AfInet = 2;
    private const int TcpTableOwnerPidConnections = 4;

    [DllImport("iphlpapi.dll", SetLastError = true)]
    private static extern uint GetExtendedTcpTable(IntPtr table, ref int size, bool order,
        int addressFamily, int tableClass, uint reserved);

    [StructLayout(LayoutKind.Sequential)]
    private struct Row
    {
        public uint State;
        public uint LocalAddress;
        public uint LocalPort;
        public uint RemoteAddress;
        public uint RemotePort;
        public int OwningProcessId;
    }

    internal static int? Find(IPEndPoint remote, int listenerPort)
    {
        if (remote.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork) return null;
        var size = 0;
        GetExtendedTcpTable(IntPtr.Zero, ref size, false, AfInet, TcpTableOwnerPidConnections, 0);
        for (var attempt = 0; attempt < 3 && size > 0; attempt++)
        {
            var buffer = Marshal.AllocHGlobal(size);
            try
            {
                var result = GetExtendedTcpTable(buffer, ref size, false, AfInet, TcpTableOwnerPidConnections, 0);
                if (result == 122) continue; // ERROR_INSUFFICIENT_BUFFER: the table grew.
                if (result != 0) return null;
                return Search(buffer, remote, listenerPort);
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        return null;
    }

    private static int? Search(IntPtr buffer, IPEndPoint remote, int listenerPort)
    {
        var count = Marshal.ReadInt32(buffer);
        var rowSize = Marshal.SizeOf<Row>();
#pragma warning disable CS0618 // IPv4 address as a network-order integer, as the table stores it.
        var address = (uint)remote.Address.Address;
#pragma warning restore CS0618
        for (var i = 0; i < count; i++)
        {
            var row = Marshal.PtrToStructure<Row>(buffer + 4 + i * rowSize);
            if (row.LocalAddress == address && Port(row.LocalPort) == remote.Port
                && Port(row.RemotePort) == listenerPort)
                return row.OwningProcessId;
        }
        return null;
    }

    // The low 16 bits hold the port in network byte order.
    private static int Port(uint value) => (int)(((value & 0xFF) << 8) | ((value >> 8) & 0xFF));
}
