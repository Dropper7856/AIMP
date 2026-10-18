using System.Text;

namespace AIMP.Launcher;

internal sealed record ScaleformBridgeMessage(
    string Type,
    IReadOnlyDictionary<string, string> Values)
{
    internal string Get(string key, string fallback = "") =>
        Values.TryGetValue(key, out var value) ? value : fallback;
}

internal sealed record ScaleformBridgeEvent(
    string Channel,
    string RawMessage,
    ScaleformBridgeMessage Message,
    Func<string, IReadOnlyDictionary<string, string>?, Task<bool>>? Reply = null,
    Guid ConnectionId = default);

internal sealed record ScaleformBridgeConnectionClosed(string Channel, Guid ConnectionId, bool PeerClosed);

internal sealed class ScaleformBridgeProtocol
{
    internal ScaleformBridgeMessage Parse(string line)
    {
        if (string.IsNullOrWhiteSpace(line))
            return new ScaleformBridgeMessage(string.Empty, new Dictionary<string, string>());
        var parts = line.Trim().Split('|');
        return new ScaleformBridgeMessage(parts[0].Trim(), ParseValues(parts));
    }

    internal string Build(string type, IReadOnlyDictionary<string, string>? values)
    {
        var builder = new StringBuilder(type?.Trim() ?? string.Empty);
        if (values is null) return builder.ToString();
        foreach (var pair in values) AppendPair(builder, pair);
        return builder.ToString();
    }

    internal string Describe(ScaleformBridgeMessage message)
    {
        if (message.Values.Count == 0) return string.Empty;
        return string.Join(", ", message.Values.OrderBy(value => value.Key,
            StringComparer.OrdinalIgnoreCase).Select(FormatPair));
    }

    private static void AppendPair(StringBuilder builder, KeyValuePair<string, string> pair)
    {
        builder.Append('|');
        builder.Append(pair.Key);
        builder.Append('=');
        builder.Append(Encode(pair.Value));
    }

    private static Dictionary<string, string> ParseValues(IReadOnlyList<string> parts)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (var index = 1; index < parts.Count; index++) AddValue(values, parts[index]);
        return values;
    }

    private static void AddValue(IDictionary<string, string> values, string part)
    {
        var equals = part.IndexOf('=');
        if (equals <= 0) return;
        var key = part[..equals].Trim();
        values[key] = Decode(part[(equals + 1)..]);
    }

    private static string FormatPair(KeyValuePair<string, string> pair) =>
        $"{pair.Key}={pair.Value}";

    internal static string Encode(string? value) => (value ?? string.Empty)
        .Replace("%", "%25", StringComparison.Ordinal)
        .Replace("|", "%7C", StringComparison.Ordinal)
        .Replace("=", "%3D", StringComparison.Ordinal)
        .Replace("\n", "%0A", StringComparison.Ordinal)
        .Replace("\r", "%0D", StringComparison.Ordinal);

    internal static string Decode(string value) => value
        .Replace("%0D", "\r", StringComparison.OrdinalIgnoreCase)
        .Replace("%0A", "\n", StringComparison.OrdinalIgnoreCase)
        .Replace("%3D", "=", StringComparison.OrdinalIgnoreCase)
        .Replace("%7C", "|", StringComparison.OrdinalIgnoreCase)
        .Replace("%25", "%", StringComparison.OrdinalIgnoreCase);

    internal static byte[] PolicyResponse() => Encoding.UTF8.GetBytes(
        "<?xml version=\"1.0\"?><cross-domain-policy>" +
        "<allow-access-from domain=\"*\" to-ports=\"12345,12346\" />" +
        "</cross-domain-policy>\0");
}
