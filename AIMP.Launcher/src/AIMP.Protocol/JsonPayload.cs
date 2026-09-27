using System.Text.Json;
using System.Text.Json.Serialization;

namespace AIMP.Protocol;

public static class JsonPayload
{
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        NumberHandling = JsonNumberHandling.Strict,
        MaxDepth = 16,
        WriteIndented = false
    };

    public static byte[] Serialize<T>(T value)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(value, Options);
        if (payload.Length > ProtocolConstants.MaximumPayloadSize)
        {
            throw new ProtocolException("The JSON document exceeds the protocol limit.");
        }

        return payload;
    }

    public static T Deserialize<T>(ReadOnlySpan<byte> payload)
    {
        if (payload.Length > ProtocolConstants.MaximumPayloadSize)
        {
            throw new ProtocolException("The JSON document exceeds the protocol limit.");
        }

        try
        {
            return JsonSerializer.Deserialize<T>(payload, Options)
                ?? throw new ProtocolException("The JSON payload is empty.");
        }
        catch (JsonException exception)
        {
            throw new ProtocolException($"Invalid JSON payload: {exception.Message}");
        }
    }
}
