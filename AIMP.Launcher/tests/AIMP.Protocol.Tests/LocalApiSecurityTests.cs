using System.Net;
using System.Text.Json;
using AIMP.Launcher;
using AIMP.Launcher.Controllers;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Primitives;

namespace AIMP.Protocol.Tests;

internal static class LocalApiSecurityTests
{
    private const string Frontend = "https://aimp.dblabs.ca";
    private const string Token = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private static readonly string[] UntrustedOrigins =
    {
        "null", "NULL", "https://attacker.invalid", "http://127.0.0.1:9000", "http://localhost:4200",
        "file://", "app://launcher", "data:text/html,hello", "https://aimp.dblabs.ca.attacker.invalid",
        "https://aimp.dblabs.ca@attacker.invalid", "https://attacker@aimp.dblabs.ca",
        "http://aimp.dblabs.ca", "https://aimp.dblabs.ca:8443", "https://aimp.dblabs.ca/path",
        "https://aimp.dblabs.ca/", "https://aimp.dblabs.ca?query", "https://aimp.dblabs.ca#fragment",
        "https://aimp.dblabs.ca https://attacker.invalid", "https://aimp.dblabs.ca, null", "", " "
    };

    internal static async Task AccessPolicyAsync()
    {
        var policy = new LocalApiAccessPolicy(Frontend, Token);
        AssertEx.True(policy.IsAllowedOrigin(Frontend));
        AssertEx.True(policy.IsAllowedOrigin("HTTPS://AIMP.DBLABS.CA"));
        AssertEx.True(new LocalApiAccessPolicy("http://localhost:4200", Token).IsAllowedOrigin("http://localhost:4200"));
        foreach (var origin in UntrustedOrigins) AssertEx.False(policy.IsAllowedOrigin(origin), origin);
        AssertEx.False(new LocalApiAccessPolicy(null, Token).IsAllowedOrigin(Frontend));
        foreach (var origin in new[] { "null", "file://", "https://user@host", Frontend + "/path" })
            AssertEx.Throws<InvalidOperationException>(() => new LocalApiAccessPolicy(origin, Token));
        foreach (var token in new[] { null, "", "short", new string('z', 64) })
            AssertEx.Throws<InvalidOperationException>(() => new LocalApiAccessPolicy(Frontend, token));

        foreach (var address in new[] { IPAddress.Loopback, IPAddress.IPv6Loopback, IPAddress.Loopback.MapToIPv6() })
        {
            var context = Context(address, "localhost");
            context.Request.Headers.Origin = Frontend;
            await policy.EnforceAsync(context, _ => { context.Response.StatusCode = 202; return Task.CompletedTask; });
            AssertEx.Equal(202, context.Response.StatusCode);
            await ((ServiceProvider)context.RequestServices).DisposeAsync();
        }
        foreach (var context in new[] { Context(IPAddress.Parse("192.0.2.1"), "localhost"), Context(null, "localhost"),
            Context(IPAddress.Loopback, "attacker.invalid") })
        {
            context.Request.Headers.Origin = Frontend;
            await AssertDeniedAsync(policy, context);
        }
        var multiple = Context(IPAddress.Loopback, "localhost");
        multiple.Request.Headers.Origin = new StringValues(new[] { Frontend, Frontend });
        await AssertDeniedAsync(policy, multiple);
        var missing = Context(IPAddress.Loopback, "localhost");
        missing.Request.Headers.Origin = "";
        await AssertDeniedAsync(policy, missing);
        var previousSession = Context(IPAddress.Loopback, "localhost");
        await AssertDeniedAsync(new LocalApiAccessPolicy(Frontend, new string('b', 64)), previousSession, 401);
    }

    internal static async Task HttpBoundaryAsync()
    {
        using var temporary = new TemporaryDirectory();
        const string filename = "private-listing-marker.txt";
        File.WriteAllText(temporary.Combine(filename), "This content must never be returned by directory browsing.");
        var policy = new LocalApiAccessPolicy(Frontend, Token);
        var builder = WebApplication.CreateBuilder(new WebApplicationOptions { EnvironmentName = "Testing" });
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Logging.ClearProviders();
        builder.Services.AddProblemDetails();
        builder.Services.AddControllers().AddApplicationPart(typeof(FileSystemController).Assembly);
        AIMP.Launcher.Program.ConfigureApiSecurityServices(builder.Services, policy);
        await using var app = builder.Build();
        AIMP.Launcher.Program.ConfigurePipeline(app, policy);
        var mutations = 0;
        app.MapMethods("/api/v1/security-test/action", new[] { "POST", "PUT" }, () => ++mutations);
        await app.StartAsync();
        var addresses = app.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!;
        using var client = new HttpClient { BaseAddress = new Uri(addresses.Addresses.Single()) };
        var directory = "/api/v1/filesystem/directory?path=" + Uri.EscapeDataString(temporary.DirectoryPath);

        foreach (var origin in UntrustedOrigins)
        {
            using var listing = await SendAsync(client, HttpMethod.Get, directory, origin);
            AssertEx.Equal(HttpStatusCode.Forbidden, listing.StatusCode, origin);
            AssertNoCors(listing);
            AssertEx.False((await listing.Content.ReadAsStringAsync()).Contains(filename, StringComparison.Ordinal));
            using var action = await SendAsync(client, HttpMethod.Post, "/api/v1/security-test/action", origin);
            AssertEx.Equal(HttpStatusCode.Forbidden, action.StatusCode, origin);
            AssertNoCors(action);
            using var shutdown = await SendAsync(client, HttpMethod.Post, "/api/v1/application/shutdown", origin);
            AssertEx.Equal(HttpStatusCode.Forbidden, shutdown.StatusCode, origin);
            using var preflight = await PreflightAsync(client, origin);
            AssertEx.Equal(HttpStatusCode.Forbidden, preflight.StatusCode, origin);
            AssertNoCors(preflight);
        }
        AssertEx.Equal(0, mutations, "Denied requests must never reach the action.");
        foreach (var origin in new[] { null, Frontend })
        foreach (var token in new[] { null, "", "short", new string('b', 64), Token + ", " + Token })
        foreach (var endpoint in new[] { (HttpMethod.Get, directory), (HttpMethod.Get, "/"),
            (HttpMethod.Options, "/"), (HttpMethod.Post, "/api/v1/security-test/action"),
            (HttpMethod.Post, "/api/v1/application/shutdown") })
        {
            using var response = await SendAsync(client, endpoint.Item1, endpoint.Item2, origin, token);
            AssertEx.Equal(HttpStatusCode.Unauthorized, response.StatusCode);
            AssertNoCors(response);
            var body = await response.Content.ReadAsStringAsync();
            AssertEx.False(body.Contains(filename, StringComparison.Ordinal));
            AssertEx.False(body.Contains(Token, StringComparison.Ordinal));
        }
        AssertEx.Equal(0, mutations, "Unauthenticated requests must never reach the action.");
        using (var listing = await SendAsync(client, HttpMethod.Get, directory, Frontend))
        {
            AssertEx.Equal(HttpStatusCode.OK, listing.StatusCode);
            AssertEx.Equal(Frontend, listing.Headers.GetValues("Access-Control-Allow-Origin").Single());
            using var body = JsonDocument.Parse(await listing.Content.ReadAsStringAsync());
            AssertEx.True(body.RootElement.GetProperty("entries").EnumerateArray()
                .Any(entry => entry.GetProperty("name").GetString() == filename));
        }
        using (var preflight = await PreflightAsync(client, Frontend))
        {
            AssertEx.Equal(HttpStatusCode.NoContent, preflight.StatusCode);
            AssertEx.Equal(Frontend, preflight.Headers.GetValues("Access-Control-Allow-Origin").Single());
            AssertEx.Equal("true", preflight.Headers.GetValues("Access-Control-Allow-Private-Network").Single());
            AssertEx.True(preflight.Headers.GetValues("Access-Control-Allow-Methods").Single().Contains("PUT"));
        }
        using (var action = await SendAsync(client, HttpMethod.Put, "/api/v1/security-test/action", Frontend))
            AssertEx.Equal(HttpStatusCode.OK, action.StatusCode);
        AssertEx.Equal(1, mutations);

        foreach (var header in new[] { "Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest", "Referer" })
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, "/api/v1/security-test/action");
            request.Headers.TryAddWithoutValidation(header, header == "Referer" ? "https://attacker.invalid/" : "cross-site");
            using var response = await client.SendAsync(request);
            AssertEx.Equal(HttpStatusCode.Forbidden, response.StatusCode);
        }
        AssertEx.Equal(1, mutations, "Origin-less browser requests must never reach the action.");
        using (var request = new HttpRequestMessage(HttpMethod.Get, directory))
        {
            request.Headers.Add("Sec-Fetch-Mode", "no-cors");
            using var response = await client.SendAsync(request);
            AssertEx.Equal(HttpStatusCode.Forbidden, response.StatusCode);
            AssertEx.False((await response.Content.ReadAsStringAsync()).Contains(filename, StringComparison.Ordinal));
        }
        using (var health = await SendAsync(client, HttpMethod.Get, "/", null)) AssertEx.Equal(HttpStatusCode.OK, health.StatusCode);
        using (var shutdown = await SendAsync(client, HttpMethod.Post, "/api/v1/application/shutdown", null))
            AssertEx.Equal(HttpStatusCode.Accepted, shutdown.StatusCode);
        await app.StopAsync();
    }

    private static DefaultHttpContext Context(IPAddress? address, string host)
    {
        var context = new DefaultHttpContext();
        context.Connection.RemoteIpAddress = address;
        context.Request.Host = new HostString(host);
        context.Request.Headers[LocalApiAccessPolicy.TokenHeader] = Token;
        // Results.Problem uses the same registered HTTP JSON services as the app.
        context.RequestServices = new ServiceCollection().AddLogging().AddProblemDetails().BuildServiceProvider();
        return context;
    }

    private static async Task AssertDeniedAsync(LocalApiAccessPolicy policy, DefaultHttpContext context, int statusCode = 403)
    {
        await policy.EnforceAsync(context, _ => throw new InvalidOperationException("Denied request reached the endpoint."));
        AssertEx.Equal(statusCode, context.Response.StatusCode);
        await ((ServiceProvider)context.RequestServices).DisposeAsync();
    }

    private static async Task<HttpResponseMessage> SendAsync(
        HttpClient client, HttpMethod method, string path, string? origin, string? token = Token)
    {
        using var request = new HttpRequestMessage(method, path);
        if (origin is not null) request.Headers.TryAddWithoutValidation("Origin", origin);
        if (token is not null) request.Headers.TryAddWithoutValidation(LocalApiAccessPolicy.TokenHeader, token);
        return await client.SendAsync(request);
    }

    private static async Task<HttpResponseMessage> PreflightAsync(HttpClient client, string origin)
    {
        using var request = new HttpRequestMessage(HttpMethod.Options, "/api/v1/security-test/action");
        request.Headers.TryAddWithoutValidation("Origin", origin);
        request.Headers.Add("Access-Control-Request-Method", "PUT");
        request.Headers.Add("Access-Control-Request-Headers", "content-type");
        request.Headers.Add("Access-Control-Request-Private-Network", "true");
        return await client.SendAsync(request);
    }

    private static void AssertNoCors(HttpResponseMessage response)
    {
        AssertEx.False(response.Headers.Contains("Access-Control-Allow-Origin"));
        AssertEx.False(response.Headers.Contains("Access-Control-Allow-Private-Network"));
    }
}
