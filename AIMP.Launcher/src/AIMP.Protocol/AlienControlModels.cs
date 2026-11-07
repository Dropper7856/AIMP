namespace AIMP.Protocol;

// The Alien player is the *decision maker*, even if they are not the lobby
// Host. The Host authenticates the origin, orders admission and broadcasts the
// same intent to every participant. Gameplay effects require a separate native
// lease / certified Character lifetime and a Host-issued readiness epoch.
public enum AlienAction : byte
{
    None = 0,
    Attack = 1,
    EnterVent = 2,
    ExitVent = 3,
    Roar = 4,
}

public sealed record AlienControlIntent(
    Guid SessionId,
    ulong AlienPeerId,
    int LevelGeneration,
    ulong Sequence,
    float Forward,
    float Strafe,
    float Yaw,
    float Pitch,
    bool Sprint,
    bool Sneak,
    AlienAction Action,
    ulong Epoch = 0,
    uint Gait = 1,
    bool Focused = true);

public sealed record AlienControlCommit(
    ulong HostSequence,
    AlienControlIntent Intent);

public static class AlienControlProtocol
{
    public static void Validate(AlienControlIntent? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.AlienPeerId == 0 ||
            value.LevelGeneration < 1 || value.Sequence == 0 || value.Epoch == 0 || value.Gait > 3 ||
            value.Sprint != (value.Gait >= 2) || value.Sneak != (value.Gait == 0) ||
            !float.IsFinite(value.Forward) || value.Forward is < -1F or > 1F ||
            !float.IsFinite(value.Strafe) || value.Strafe is < -1F or > 1F ||
            !float.IsFinite(value.Yaw) || value.Yaw is < -180F or > 180F ||
            !float.IsFinite(value.Pitch) || value.Pitch is < -90F or > 90F ||
            (byte)value.Action > (byte)AlienAction.Roar ||
            (value.Sprint && value.Sneak) ||
            (!value.Focused && (value.Forward!=0 || value.Strafe!=0 || value.Action!=AlienAction.None)))
            throw new ProtocolException("Alien control contains an invalid actor identity or input.");
    }

    public static void Validate(AlienControlCommit? value)
    {
        if (value is null || value.HostSequence == 0)
            throw new ProtocolException("Alien control commit has no Host ordering identity.");
        Validate(value.Intent);
    }
}
