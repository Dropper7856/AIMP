using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class RemoteEquipmentSelectionTests
{
    internal static Task SelectionAsync()
    {
        // Slot 9 is shared by multiple item archetypes, including throwables.
        foreach (var current in new[] { -1, 7, 8, 9, 11, 64 })
        foreach (var active in new[] { false, true })
        {
            AssertEx.Equal(current, RemoteEquipmentSelection.VisibleIndex(current, 1, active));
            AssertEx.False(RemoteEquipmentSelection.SupportsActions(current, active));
        }
        AssertEx.Equal(1, RemoteEquipmentSelection.VisibleIndex(10, 1, false));
        // Raw aim/primary and the visible gun can remain identical while its
        // active bit drops. This transition must still publish/converge Stop.
        AssertEx.True(RemoteEquipmentSelection.ActionEligibilityChanged(1, true, 1, false));
        AssertEx.True(RemoteEquipmentSelection.ActionEligibilityChanged(1, false, 1, true));
        AssertEx.False(RemoteEquipmentSelection.ActionEligibilityChanged(1, true, 1, true));
        AssertEx.False(RemoteEquipmentSelection.ActionEligibilityChanged(9, true, 9, false));
        AssertEx.Equal(10, RemoteEquipmentSelection.VisibleIndex(10, 1, true));
        AssertEx.Equal(10, RemoteEquipmentSelection.VisibleIndex(10, 9, false));
        AssertEx.False(RemoteEquipmentSelection.SupportsActions(10, false));
        foreach (var current in Enumerable.Range(0, 7))
        {
            AssertEx.Equal(current, RemoteEquipmentSelection.VisibleIndex(current, 1, false));
            AssertEx.True(RemoteEquipmentSelection.SupportsActions(current, true));
            AssertEx.False(RemoteEquipmentSelection.SupportsActions(current, false));
        }
        // Gun -> item -> holster(previous gun) must produce distinct selections.
        AssertEx.Equal(9, RemoteEquipmentSelection.VisibleIndex(9, 1, true));
        AssertEx.Equal(1, RemoteEquipmentSelection.VisibleIndex(10, 1, false));
        return Task.CompletedTask;
    }
}
