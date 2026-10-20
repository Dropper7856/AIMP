namespace AIMP.Launcher;

// Owned by the coordinator's state lock. Returning to the lobby resets the
// current world but must not reuse an authority incarnation in the same lobby.
internal sealed class SessionWorldGenerationCounter
{
    private Guid _session;
    private int _last;

    internal int Next(Guid session)
    {
        if (session == Guid.Empty) throw new ArgumentException("Session is required.", nameof(session));
        if (_session != session) { _session = session; _last = 0; }
        return _last = checked(_last + 1);
    }
}
