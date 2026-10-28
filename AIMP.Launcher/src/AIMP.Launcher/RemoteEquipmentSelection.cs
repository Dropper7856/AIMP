namespace AIMP.Launcher;

// A carried gun can survive the native holster transition. An unknown active
// selection cannot borrow that gun: slot 9 represents several different items.
internal static class RemoteEquipmentSelection
{
    internal static bool SupportsActions(int current, bool active) =>
        active && current is not (32 or 33)
        && RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(current, out _);

    internal static bool ActionEligibilityChanged(int previousCurrent, bool previousActive, int current, bool active) =>
        SupportsActions(previousCurrent, previousActive) != SupportsActions(current, active);

    internal static int VisibleIndex(int current, int previous, bool active)
    {
        if (RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(current, out _)) return current;
        return current == 10 && !active
            && RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(previous, out _)
                ? previous : current;
    }
}
