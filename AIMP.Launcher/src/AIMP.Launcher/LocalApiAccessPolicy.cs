using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace AIMP.Launcher;

internal sealed class LocalApiAccessPolicy
{
    private readonly string? _frontendOrigin;
    private readonly byte[] _apiToken;
    internal const string TokenHeader = "X-AIMP-Token";

    internal LocalApiAccessPolicy(string? frontendOrigin, string? apiToken)
    {
        if (apiToken is null || apiToken.Length != 64 || !apiToken.All(Uri.IsHexDigit))
            throw new InvalidOperationException("AIMP_API_TOKEN must be a 32-byte hexadecimal session token supplied by Electron.");
        _apiToken = Encoding.UTF8.GetBytes(apiToken);
        if (string.IsNullOrWhiteSpace(frontendOrigin)) return;
        _frontendOrigin = ParseOrigin(frontendOrigin.Trim().TrimEnd('/'))
            ?? throw new InvalidOperationException("AIMP_FRONTEND_ORIGIN must be a single HTTP or HTTPS origin.");
    }

    internal bool IsAllowedOrigin(string origin) =>
        _frontendOrigin is not null
        && ParseOrigin(origin) is { } parsed
        && string.Equals(parsed, _frontendOrigin, StringComparison.OrdinalIgnoreCase);

    internal Task EnforceAsync(HttpContext context, RequestDelegate next)
    {
        if (!CanAccess(context)) return DenyAsync(context);
        var preflight = IsCorsPreflight(context.Request);
        if (!preflight && !HasSessionToken(context.Request))
            return DenyAsync(context, StatusCodes.Status401Unauthorized);
        if (preflight && string.Equals(context.Request.Headers["Access-Control-Request-Private-Network"], "true",
                StringComparison.OrdinalIgnoreCase))
            context.Response.Headers["Access-Control-Allow-Private-Network"] = "true";
        return next(context);
    }

    private bool CanAccess(HttpContext context)
    {
        var remote = context.Connection.RemoteIpAddress;
        if (remote is null || !IPAddress.IsLoopback(remote.IsIPv4MappedToIPv6 ? remote.MapToIPv4() : remote)
            || !IsLoopbackHost(context.Request.Host.Host))
            return false;
        var headers = context.Request.Headers;
        if (headers.TryGetValue("Origin", out var origins))
            return origins.Count == 1 && IsAllowedOrigin(origins[0]!);
        // Native clients still need the session token. Browser requests that
        // omit Origin must not impersonate native clients using no-cors fetches.
        return !headers.ContainsKey("Referer")
            && !headers.Keys.Any(name => name.StartsWith("Sec-Fetch-", StringComparison.OrdinalIgnoreCase));
    }

    private bool HasSessionToken(HttpRequest request) =>
        request.Headers.TryGetValue(TokenHeader, out var tokens) && tokens.Count == 1
        && tokens[0] is { Length: 64 } token
        && CryptographicOperations.FixedTimeEquals(_apiToken, Encoding.UTF8.GetBytes(token));

    private static bool IsCorsPreflight(HttpRequest request) =>
        HttpMethods.IsOptions(request.Method) && request.Headers.ContainsKey("Origin")
        && request.Headers["Access-Control-Request-Method"] is { Count: 1 } methods
        && !string.IsNullOrWhiteSpace(methods[0]);

    private static bool IsLoopbackHost(string host) =>
        string.Equals(host, "localhost", StringComparison.OrdinalIgnoreCase)
        || (IPAddress.TryParse(host.Trim('[', ']'), out var address) && IPAddress.IsLoopback(address));

    private static string? ParseOrigin(string origin)
    {
        if (!Uri.TryCreate(origin, UriKind.Absolute, out var uri)
            || (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)
            || uri.UserInfo.Length != 0 || uri.AbsolutePath != "/"
            || uri.Query.Length != 0 || uri.Fragment.Length != 0)
            return null;

        // Reject paths, trailing slashes, whitespace and other non-origin URI
        // forms instead of silently normalizing attacker-controlled input.
        var authority = uri.GetLeftPart(UriPartial.Authority);
        return string.Equals(origin, authority, StringComparison.OrdinalIgnoreCase)
            ? authority : null;
    }

    private static Task DenyAsync(HttpContext context, int statusCode = StatusCodes.Status403Forbidden)
    {
        context.Response.Headers.CacheControl = "no-store";
        return Results.Problem(statusCode: statusCode, title: "Local API access denied.")
            .ExecuteAsync(context);
    }
}
