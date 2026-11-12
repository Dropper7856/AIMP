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
