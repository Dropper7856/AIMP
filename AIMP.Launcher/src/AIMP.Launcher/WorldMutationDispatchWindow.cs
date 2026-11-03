namespace AIMP.Launcher;

// Used under the coordinator's state lock. Admission may run ahead of native
// commit, but durable checkpoints still advance only from ordered native results.
internal sealed class WorldMutationDispatchWindow(int capacity)
{
    private sealed record Entry(ulong End, int Roots, TaskCompletionSource<bool> Completion);
    private readonly Queue<Entry> _pending = new();
    private int _roots;
    internal int PendingRoots { get { Prune(); return _roots; } }
    internal int HighWater { get; private set; }

    private void Prune()
    {
        while (_pending.TryPeek(out var entry) && entry.Completion.Task.IsCompleted)
        {
            _pending.Dequeue();
            _roots -= entry.Roots;
        }
    }

    internal Task<bool>? CapacityWait(int roots)
    {
        if (roots <= 0 || roots > capacity) throw new ArgumentOutOfRangeException(nameof(roots));
        Prune();
        return _roots + roots <= capacity ? null : _pending.Peek().Completion.Task;
    }

    internal ulong NextSequence(ulong committed)
    {
        Prune();
        return _pending.Count == 0 ? committed + 1U : _pending.Last().End + 1U;
    }

    internal void Add(ulong start, int roots, TaskCompletionSource<bool> completion)
    {
        if (CapacityWait(roots) is not null) throw new InvalidOperationException("World dispatch window full.");
        _pending.Enqueue(new Entry(checked(start + (ulong)roots - 1U), roots, completion));
        _roots += roots;
        HighWater = Math.Max(HighWater, _roots);
    }

    internal bool Contains(Task<bool> completion) => _pending.Any(e => e.Completion.Task == completion);

    internal void Clear()
    {
        foreach (var entry in _pending) entry.Completion.TrySetCanceled();
        _pending.Clear();
        _roots = 0;
        HighWater = 0;
    }
}
