using System.Globalization;
using System.Text.Json;
using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private int _alienVentPollRunning;
    private DateTimeOffset _nextAlienVentPoll;
    private uint _alienVentPage;
    private string _alienVentScope=string.Empty;
    private bool _alienVentHudVisible;
    private Task _alienVentPollTask=Task.CompletedTask;

    private void TryScheduleAlienVentMap(NativeRuntimeSnapshot runtime,DateTimeOffset now)
    {
        if(Volatile.Read(ref _disposed)!=0 || now<_nextAlienVentPoll || Interlocked.CompareExchange(ref _alienVentPollRunning,1,0)!=0)return;
        _nextAlienVentPoll=now.AddMilliseconds(50);
        _alienVentPollTask=PollAlienVentMapAsync();
    }
    private async Task PollAlienVentMapAsync()
    {
        try
        {
            var binding=CaptureAlienPvPNetworkBinding();
            if(!binding.Active || binding.LocalPeer!=binding.OwnerPeer)
            { await HideAlienVentMapAsync().ConfigureAwait(false);return; }
            var result=await _controller.ExecuteCommandAsync(GameCommandNames.AlienPvPVents,
                new Dictionary<string,string>{{"page",_alienVentPage.ToString(CultureInfo.InvariantCulture)}},
                _lifetime.Token,waitForExecution:true,expectedLevelGeneration:binding.NativeGeneration).ConfigureAwait(false);
            var current=CaptureAlienPvPNetworkBinding();
            if(current!=binding || !result.Executed)
            { await HideAlienVentMapAsync().ConfigureAwait(false);return; }
            var marker=result.Message.IndexOf("AVSVENTS:",StringComparison.Ordinal);
            if(marker<0){await HideAlienVentMapAsync().ConfigureAwait(false);return;}
            using var document=JsonDocument.Parse(result.Message[(marker+9)..]);
            var root=document.RootElement;
            var scope=root.GetProperty("scope").GetString()??string.Empty;
            if(!scope.StartsWith($"{AISyncCodec.SessionCookie(binding.Session)}:{binding.OwnerPeer}:{binding.NativeGeneration}:{binding.WorldGeneration}:",StringComparison.Ordinal))
                return;
            var count=root.GetProperty("count").GetUInt32();
            if(count>4096)throw new ProtocolException("Alien vent catalogue exceeds its native limit.");
            if(scope!=_alienVentScope){_alienVentScope=scope;_alienVentPage=0;}
            else _alienVentPage=(_alienVentPage+1)%Math.Max(1U,(count+63)/64);
            static string Number(JsonElement value)
            {
                var number=value.GetSingle();
                if(!float.IsFinite(number) || Math.Abs(number)>100000)throw new ProtocolException("Invalid vent coordinate.");
                return number.ToString("R",CultureInfo.InvariantCulture);
            }
            var points=new List<string>();
            foreach(var point in root.GetProperty("vents").EnumerateArray())
            {
                if(point.GetArrayLength()!=4 || point[0].GetUInt32()>=count || points.Count>=64)
                    throw new ProtocolException("Invalid vent catalogue page.");
                points.Add($"{point[0].GetUInt32()},{Number(point[1])},{Number(point[2])},{Number(point[3])}");
            }
            // Only the current local Alien's gameplay HUD receives this data.
            if(CaptureAlienPvPNetworkBinding()!=binding)return;
            var players=new List<string>();
            lock(_stateGate)
            {
                var palette=new[]{"79E9DF","F3C96A","E78CDA","8EB8FF"};
                var color=0;
                foreach(var player in (_lobby?.Players??Array.Empty<LobbyPlayer>()).OrderBy(p=>p.PeerId))
                {
                    var tint=palette[color++%palette.Length];
                    if(player.PeerId==binding.OwnerPeer || !_remotePositions.TryGetValue(player.PeerId,out var track))continue;
                    var snapshot=track.Snapshot();
                    var pose=snapshot?.Latest;
                    if(pose is null || pose.SessionId!=binding.Session || pose.LevelGeneration!=binding.RoundGeneration ||
                        snapshot is null || DateTimeOffset.UtcNow-snapshot.ReceivedAtUtc>TimeSpan.FromSeconds(2))continue;
                    players.Add(string.Join(',',player.PeerId.ToString(CultureInfo.InvariantCulture),tint,
                        pose.X.ToString("R",CultureInfo.InvariantCulture),pose.Y.ToString("R",CultureInfo.InvariantCulture),
                        pose.Z.ToString("R",CultureInfo.InvariantCulture),Uri.EscapeDataString(player.DisplayName)));
                }
            }
            await _bridge.SendAsync("ALIEN_VENTS",new Dictionary<string,string>
            {
                ["active"]="1",["scope"]=scope,["count"]=count.ToString(CultureInfo.InvariantCulture),
                ["controlReady"]=root.GetProperty("controlReady").GetBoolean()?"1":"0",
                ["nearby"]=root.GetProperty("nearby").GetInt32().ToString(CultureInfo.InvariantCulture),
                ["selected"]=root.GetProperty("selected").GetInt32().ToString(CultureInfo.InvariantCulture),
                ["open"]=root.GetProperty("open").GetBoolean()?"1":"0",
                ["backstage"]=root.GetProperty("backstage").GetBoolean()?"1":"0",
                ["action"]=root.GetProperty("action").GetInt32().ToString(CultureInfo.InvariantCulture),
                ["phase"]=root.GetProperty("phase").GetInt32().ToString(CultureInfo.InvariantCulture),
                ["pending"]=root.GetProperty("pending").GetBoolean()?"1":"0",
                ["x"]=Number(root.GetProperty("x")),["y"]=Number(root.GetProperty("y")),["z"]=Number(root.GetProperty("z")),
                ["points"]=string.Join(';',points),["players"]=string.Join(';',players),
                ["maps"]=string.Join(',',root.GetProperty("maps").EnumerateArray().Select(v=>v.GetString()??string.Empty)),
                ["projection"]=string.Join(',',root.GetProperty("projection").EnumerateArray().Select(Number)),
                ["mx"]=Number(root.GetProperty("mx")),["my"]=Number(root.GetProperty("my")),
                ["wheel"]=root.GetProperty("wheel").GetInt32().ToString(CultureInfo.InvariantCulture),
                ["down"]=root.GetProperty("down").GetBoolean()?"1":"0",
                ["click"]=root.GetProperty("click").GetBoolean()?"1":"0"
            },_lifetime.Token).ConfigureAwait(false);
            _alienVentHudVisible=true;
        }
        catch(OperationCanceledException) when(_lifetime.IsCancellationRequested) { }
        catch(Exception error)
        {
            _nextAlienVentPoll=DateTimeOffset.UtcNow.AddSeconds(2);
            _diagnostics.Log("AlienPvP","Vent map deferred: "+error.Message);
            await HideAlienVentMapAsync().ConfigureAwait(false);
        }
        finally {Interlocked.Exchange(ref _alienVentPollRunning,0);}
    }
    private async Task GoToAlienVentAsync(ScaleformBridgeMessage message)
    {
        var scope=message.Get("scope","");
        var binding=CaptureAlienPvPNetworkBinding();
        if(!binding.Active || binding.LocalPeer!=binding.OwnerPeer || scope!=_alienVentScope ||
            !scope.StartsWith($"{AISyncCodec.SessionCookie(binding.Session)}:{binding.OwnerPeer}:{binding.NativeGeneration}:{binding.WorldGeneration}:",StringComparison.Ordinal) ||
            !uint.TryParse(message.Get("vent",""),NumberStyles.None,CultureInfo.InvariantCulture,out var index) || index>=4096)return;
        var serial=scope[(scope.LastIndexOf(':')+1)..];
        if(!ulong.TryParse(serial,NumberStyles.None,CultureInfo.InvariantCulture,out var incarnation)||incarnation==0)return;
        await _controller.ExecuteCommandAsync(GameCommandNames.AlienPvPVents,
            new Dictionary<string,string>{{"page","0"},{"go",index.ToString(CultureInfo.InvariantCulture)},{"incarnation",serial}},
            _lifetime.Token,waitForExecution:true,expectedLevelGeneration:binding.NativeGeneration).ConfigureAwait(false);
    }
    private async Task HideAlienVentMapAsync()
    {
        _alienVentScope=string.Empty;_alienVentPage=0;
        if(!_alienVentHudVisible)return;
        _alienVentHudVisible=false;
        try {await _bridge.SendAsync("ALIEN_VENTS",new Dictionary<string,string>{{"active","0"}},_lifetime.Token).ConfigureAwait(false);}
        catch(OperationCanceledException) when(_lifetime.IsCancellationRequested) { }
        catch(Exception error) {_diagnostics.Log("AlienPvP","Vent HUD clear deferred: "+error.Message);}
    }
}
