using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

// A web page can POST to 127.0.0.1:12345; its body lines must never run as
// menu commands. The game's own first messages must still be accepted.
internal static class BridgeSecurityTests
{
    internal static Task RejectsBrowserTrafficAsync()
    {
        foreach (var line in new[]
                 {
                     "POST / HTTP/1.1", "GET /?x=1 HTTP/1.1", "OPTIONS * HTTP/1.1",
                     "get / http/1.0", "\u0016\u0003\u0001\u0002", "JOIN_SERVER|serverId=1\u0001"
                 })
            AssertEx.True(ScaleformBridgeListener.LooksForeign(line), line);
        return Task.CompletedTask;
    }

    internal static Task AcceptsGameTrafficAsync()
    {
        foreach (var line in new[]
                 {
                     "HELLO|client=gfx|version=3", "MENU_STATE|client=gfx", "REFRESH_FRIENDS",
                     "HELLO|source=challenge_map_overlay|phase=gameplay", "CREATE_LOBBY|visibility=private\tx",
                     "JOIN_SERVER|serverId=109775241000000001"
                 })
            AssertEx.True(!ScaleformBridgeListener.LooksForeign(line), line);
        return Task.CompletedTask;
    }
}
