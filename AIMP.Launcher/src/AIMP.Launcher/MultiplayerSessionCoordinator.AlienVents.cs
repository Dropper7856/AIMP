using System.Globalization;
using System.Text.Json;
using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private int _alienVentPollRunning;
    private DateTimeOffset _nextAlienVentPoll;
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
                new Dictionary<string,string>{{"page","0"}},
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
            if(CaptureAlienPvPNetworkBinding()!=binding)return;
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
                ["freeCamera"]=root.GetProperty("freeCamera").GetBoolean()?"1":"0"
            },_lifetime.Token).ConfigureAwait(false);
            _alienVentHudVisible=true;
        }
        catch(OperationCanceledException) when(_lifetime.IsCancellationRequested) { }
        catch(Exception error)
        {
            _nextAlienVentPoll=DateTimeOffset.UtcNow.AddSeconds(2);
            _diagnostics.Log("AlienPvP","Vent interaction deferred: "+error.Message);
            await HideAlienVentMapAsync().ConfigureAwait(false);
        }
        finally {Interlocked.Exchange(ref _alienVentPollRunning,0);}
    }
    private async Task HideAlienVentMapAsync()
    {
        if(!_alienVentHudVisible)return;
        _alienVentHudVisible=false;
        try {await _bridge.SendAsync("ALIEN_VENTS",new Dictionary<string,string>{{"active","0"}},_lifetime.Token).ConfigureAwait(false);}
        catch(OperationCanceledException) when(_lifetime.IsCancellationRequested) { }
        catch(Exception error) {_diagnostics.Log("AlienPvP","Vent HUD clear deferred: "+error.Message);}
    }
}
