using System.Text.Json;
using System.Text.Json.Serialization;

namespace AIMP.Launcher;

/// <summary>One JSON object per line on the Steam helper's stdin/stdout. The pipes are
/// private to the launcher and its child, so this adds no network surface.
/// Requests carry <see cref="Op"/> and an id; replies echo the id; helper
/// notifications (log, join, invite) carry <see cref="Event"/>.</summary>
internal sealed record SteamHostMessage
{
    public long Id { get; init; }
    public string? Op { get; init; }
    public string? Event { get; init; }
    public bool Ok { get; init; }
    public string? Error { get; init; }
    public string? ErrorKind { get; init; }
    public ulong SteamId { get; init; }
    public JsonElement? Data { get; init; }
}

internal sealed record SteamHostHosting(int Port, string Status, SteamLobbyVisibility Visibility, SteamLobbyAdvert? Advert);

internal static class SteamHostProtocol
{
    internal const string Argument = "--aimp-steam-host";

    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web)
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    internal static string Serialize(SteamHostMessage message) => JsonSerializer.Serialize(message, Json);

    internal static SteamHostMessage Parse(string line) =>
        JsonSerializer.Deserialize<SteamHostMessage>(line, Json)
        ?? throw new InvalidDataException("Empty Steam helper message.");

    internal static JsonElement ToElement(object value) => JsonSerializer.SerializeToElement(value, value.GetType(), Json);

    internal static T Read<T>(JsonElement? data) =>
        data is { ValueKind: not JsonValueKind.Null } element
            ? element.Deserialize<T>(Json) ?? throw new InvalidDataException("Invalid Steam helper data.")
            : throw new InvalidDataException("Missing Steam helper data.");

    // Exception types the coordinator distinguishes survive the process boundary.
    internal static string KindOf(Exception exception) => exception switch
    {
        TimeoutException => nameof(TimeoutException),
        OperationCanceledException => nameof(OperationCanceledException),
        ArgumentException => nameof(ArgumentException),
        ObjectDisposedException => nameof(ObjectDisposedException),
        IOException => nameof(IOException),
        _ => nameof(InvalidOperationException)
    };

    internal static Exception ToException(SteamHostMessage reply)
    {
        var message = string.IsNullOrWhiteSpace(reply.Error) ? "The Steam helper reported an error." : reply.Error;
        return reply.ErrorKind switch
        {
            nameof(TimeoutException) => new TimeoutException(message),
            nameof(OperationCanceledException) => new OperationCanceledException(message),
            nameof(ArgumentException) => new ArgumentException(message),
            nameof(ObjectDisposedException) => new ObjectDisposedException(null, message),
            nameof(IOException) => new IOException(message),
            _ => new InvalidOperationException(message)
        };
    }
}
