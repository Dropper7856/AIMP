using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class WorldDispatchWindowTests
{
    internal static Task CapacityAndOrderAsync()
    {
        var window = new WorldMutationDispatchWindow(128);
        var pending = new Queue<(ulong End, TaskCompletionSource<bool> Completion)>();
        ulong committed = 0;
        // Sustained load must admit many transactions before the first reply,
        // while keeping a bounded number of ROOTs and contiguous admission.
        for (ulong sequence = 1; sequence <= 10000; sequence++)
        {
            if (window.CapacityWait(1) is { } wait)
            {
                AssertEx.Equal(128, window.PendingRoots);
                AssertEx.True(ReferenceEquals(wait, pending.Peek().Completion.Task));
                var head = pending.Dequeue();
                committed = head.End;
                head.Completion.SetResult(true);
            }
            AssertEx.Equal(sequence, window.NextSequence(committed));
            var completion = new TaskCompletionSource<bool>();
            window.Add(sequence, 1, completion);
            pending.Enqueue((sequence, completion));
        }
        AssertEx.Equal(128, window.HighWater);
        window.Clear();
        AssertEx.True(pending.All(p => p.Completion.Task.IsCanceled));
        AssertEx.Equal(0, window.PendingRoots);
        AssertEx.Equal(1UL, window.NextSequence(0));

        // Capacity counts members, including the largest legal transactions.
        var first = new TaskCompletionSource<bool>();
        var second = new TaskCompletionSource<bool>();
        window.Add(1, 32, first);
        window.Add(33, 32, second);
        window.Add(65, 32, new TaskCompletionSource<bool>());
        window.Add(97, 32, new TaskCompletionSource<bool>());
        second.SetResult(true);
        AssertEx.Equal(128, window.PendingRoots, "A later completion cannot release the FIFO head.");
        AssertEx.True(ReferenceEquals(first.Task, window.CapacityWait(32)));
        AssertEx.Equal(129UL, window.NextSequence(0));
        first.SetResult(true);
        AssertEx.Equal(64, window.PendingRoots);
        AssertEx.True(window.CapacityWait(32) is null);
        window.Clear();
        return Task.CompletedTask;
    }
}
