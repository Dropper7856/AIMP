using AIMP.Launcher;
using System.Reflection;

namespace AIMP.Protocol.Tests;

internal static partial class IpcTests
{
    internal static async Task AISyncReconfigureOnReconnectAsync()
    {
        using var directory=new TemporaryDirectory();
        using var diagnostics=new LauncherDiagnostics(directory.Combine("logs"));
        using var controller=new NativeRuntimeController(diagnostics,new LauncherSettingsStore(directory.Combine("settings.json")));
        using var ai=new AISyncSession(controller,new PerformanceIsolationState());
        SetField(controller,"_process",System.Diagnostics.Process.GetCurrentProcess());
        using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(20));var token=timeout.Token;
        var executable=directory.Combine("AI.exe");var image=new byte[]{0x4D,0x5A,1,2,3};
        await File.WriteAllBytesAsync(executable,image,token);
        var sessionId=Guid.NewGuid();Guid previous=Guid.Empty;long previousEpoch=-1;
        for(var incarnation=0;incarnation<2;incarnation++)
        {
            await using var ipc=new DllIpcSession(Environment.ProcessId);
            var identity=Guid.NewGuid();
            var handshake=ipc.AcceptAndHandshakeAsync(executable,identity,TimeSpan.FromSeconds(5),token);
            using var pipe=CreateClient();await pipe.ConnectAsync(token);
            await SendHandshakeAsync(pipe,image,identity,new List<MessageType>(),token,aiSyncAvailable:true);
            await handshake;
            SetField(controller,"_ipc",ipc);
            SetField(controller,"_snapshot",NativeRuntimeSnapshot.Empty with {
                AiRunning=true,DllInjected=true,IpcConnected=true,ProcessId=Environment.ProcessId,LevelGeneration=7 });
            if(incarnation!=0)
            {
                AssertEx.False(previous==ipc.ConnectionIdentity);
                var binding=(AISyncHostBinding)typeof(AISyncSession).GetMethod("CaptureHostBinding",BindingFlags.NonPublic|BindingFlags.Instance)!.Invoke(ai,null)!;
                AssertEx.False(binding.Active,"A fresh pipe borrowed the previous configuration grant.");
                await AssertEx.ThrowsAsync<InvalidOperationException>(()=>controller.SendAISyncConfigurationAsync(previous,
                    Environment.ProcessId,AISyncCodec.Configuration(1,7,1,sessionId,1,new Dictionary<ulong,uint>()),token));
            }
            await ai.ConfigureAsync(sessionId,true,7,1,true,true,1,token);
            var frame=await ReadExpectedAsync(pipe,MessageType.AISyncConfigure,new List<MessageType>(),token);
            AssertEx.SequenceEqual(AISyncCodec.Configuration(1,7,1,sessionId,1,new Dictionary<ulong,uint>()),frame.Payload.ToArray());
            var epoch=GetField<long>(ai,"_epoch");AssertEx.True(epoch>previousEpoch);
            AssertEx.Equal(ipc.ConnectionIdentity,GetField<Guid>(ai,"_configurationConnectionIdentity"));
            previous=ipc.ConnectionIdentity;previousEpoch=epoch;SetField(controller,"_ipc",null);
        }
    }

    private static T GetField<T>(object target,string name)=>(T)target.GetType()
        .GetField(name,BindingFlags.NonPublic|BindingFlags.Instance)!.GetValue(target)!;
    private static void SetField(object target,string name,object? value)=>target.GetType()
        .GetField(name,BindingFlags.NonPublic|BindingFlags.Instance)!.SetValue(target,value);
}
