using AIMP.Protocol;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace AIMP.Launcher;

// A deliberately narrow observability seam for TestLab. The receipt captures
// evidence at specific Launcher/runtime boundaries, not animation rendering or
// the final CATHODE mission state. Hashes are *join identifiers*, NOT signatures.
internal static class MissionTestLabReceipts
{
    private static string Hash(string text) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)).AsSpan(0, 16));

    // This is *independent* of a MissionSync commit: PlayerLifecycle observes
    // a retail native mission-terminal or level-exit boundary. It cannot be
    // attributed to any particular frontier, and does not prove HUD rendering.
    internal static string FormatNativeTerminal(Guid sessionId, int generation, string role, ulong nativeSequence)
    {
        if (sessionId == Guid.Empty || generation <= 0 || nativeSequence == 0)
            throw new ArgumentOutOfRangeException(nameof(nativeSequence), "A live session, generation and native edge are required.");
        if (role is not ("host" or "client"))
            throw new ArgumentException("Invalid TestLab peer role.", nameof(role));
        return FormattableString.Invariant(
            $"TLO1 v=1 module=PlayerLifecycle stage=native-terminal-boundary session={Hash(sessionId.ToString("N"))} generation={generation} role={role} native_seq={nativeSequence}");
    }

    internal static string Format(MissionSignalPayload signal, string role, string stage)
    {
        MissionSignalValidation.Validate(signal);
        if (role is not ("host" or "client"))
            throw new ArgumentException("Invalid TestLab peer role.", nameof(role));
        if (stage is not ("native-produced" or "commit-observed"
            or "apply-attempted" or "native-command-executed"))
            throw new ArgumentException("Invalid TestLab receipt stage.", nameof(stage));

        // Host proposal sequence is local; committed sequence is Host-global.
        // Therefore identity cannot use the Sequence field. Normalize the
        // source, exact frontier and authored occurrence instead.
        var operation = FormattableString.Invariant(
            $"{signal.SessionId:N}|{signal.LevelGeneration}|{signal.PeerId}|{signal.EntityId.ToUpperInvariant()}|{signal.Signal}|{signal.InterfaceSelector}|{signal.Occurrence}");
        var order = stage == "native-produced" ? 0UL : signal.Sequence;
        return FormattableString.Invariant(
            $"TLR1 v=1 module=MissionSync stage={stage} session={Hash(signal.SessionId.ToString("N"))} generation={signal.LevelGeneration} operation={Hash(operation)} role={role} order={order}");
    }
}
