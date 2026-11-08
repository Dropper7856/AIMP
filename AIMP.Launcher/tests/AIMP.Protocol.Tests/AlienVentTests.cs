namespace AIMP.Protocol.Tests;

internal static class AlienVentTests
{
    internal static Task OwnerStageRecoveryAsync()
    {
        var session=Guid.NewGuid();
        var authority=new AlienControlAuthority();authority.Begin(session,1,2);
        foreach(var peer in new ulong[]{1,2})
            AssertEx.True(authority.Observe(new(session,1,peer,2,1,Guid.NewGuid(),11,12,true),1000));
        var lease=authority.Evaluate(new ulong[]{1,2},1000)!;
        var pose=new AlienControlPose(session,1,2,lease.Epoch,1,0,0,11,12,0,0,0,0,0,0,1,false);
        AssertEx.True(authority.AdmitPose(pose,1000));
        var entered=pose with {Sequence=2,HostSequence=1,AcknowledgedInput=1,X=60,Y=4,Backstage=true};
        AssertEx.True(authority.AdmitOwnerPose(entered,1100) is null); // no authorized traversal
        var intent=new AlienControlIntent(session,2,1,1,0,0,0,0,false,false,AlienAction.EnterVent,lease.Epoch,1,true);
        var commit=authority.Admit(intent,1100)!;
        var started=new AlienActionResolution(session,1,2,lease.Epoch,1,commit.HostSequence,AlienAction.EnterVent,1,
            AlienActionPhase.Started,AlienActionReason.None,11,12,0,0,0,0,60,0,0,60,4,0);
        AssertEx.True(authority.AdmitResolution(started,1200));
        AssertEx.True(authority.AdmitOwnerPose(entered with {NativeBusy=true},1500) is null);
        AssertEx.True(authority.AdmitOwnerPose(entered with {X=80},1500) is null); // wrong portal
        AssertEx.True(authority.AdmitOwnerPose(entered with {HostSequence=0},1500) is null); // not applied
        AssertEx.True(authority.AdmitResolution(started with {Revision=2,Phase=AlienActionPhase.Failed,
            Reason=AlienActionReason.Timeout},6200));
        var accepted=authority.AdmitOwnerPose(entered,6300);
        AssertEx.True(accepted is {Backstage:true,OwnerMeasured:true});
        AssertEx.True(authority.AdmitOwnerPose(entered,6400) is null); // duplicate

        var exit=authority.Admit(intent with {Sequence=2,Action=AlienAction.ExitVent,Vent=new(100,0,0)},6400)!;
        var exiting=started with {InputSequence=2,HostSequence=exit.HostSequence,Action=AlienAction.ExitVent,
            X=100,Y=0,Z=0,ResolvedX=100,ResolvedY=0,ResolvedZ=0};
        AssertEx.True(authority.AdmitResolution(exiting,6500));
        var outside=entered with {Sequence=3,HostSequence=exit.HostSequence,AcknowledgedInput=2,X=100,Y=0,Backstage=false};
        AssertEx.True(authority.AdmitOwnerPose(outside,33501) is null); // expired start proof
        AssertEx.True(authority.AdmitResolution(exiting with {Revision=2,Phase=AlienActionPhase.Completed},33502));
        AssertEx.True(authority.AdmitOwnerPose(outside,33503) is {Backstage:false,OwnerMeasured:true});
        return Task.CompletedTask;
    }
    internal static Task TransportAsync()
    {
        var session=Guid.NewGuid();
        var intent=new AlienControlIntent(session,2,1,4,0,0,0,0,false,false,AlienAction.ExitVent,1,1,true,new(10,22,40));
        AlienControlProtocol.Validate(intent);
        var decoded=JsonPayload.Deserialize<AlienControlIntent>(JsonPayload.Serialize(intent));
        AssertEx.Equal(intent,decoded);
        AssertEx.Throws<ProtocolException>(()=>AlienControlProtocol.Validate(intent with {Action=AlienAction.Attack}));
        AssertEx.Throws<ProtocolException>(()=>AlienControlProtocol.Validate(intent with {Vent=new(float.NaN,0,0)}));
        var frame=new AlienPvPNativeFrame {Kind=AlienPvPNativeKind.Input,Scope=new(1,2,3,4,5,6,7),
            Epoch=1,Sequence=4,Flags=AlienPvPNativeFlags.Focused|AlienPvPNativeFlags.VentSelected,
            Action=AlienAction.ExitVent,X=10,Y=22,Z=40};
        AssertEx.Equal(frame,AlienPvPNativeCodec.Decode(AlienPvPNativeCodec.Encode(frame)));
        var entry=frame with {Action=AlienAction.EnterVent};
        AssertEx.Equal(entry,AlienPvPNativeCodec.Decode(AlienPvPNativeCodec.Encode(entry)));
        var entryIntent=intent with {Action=AlienAction.EnterVent};
        AlienControlProtocol.Validate(entryIntent);
        AssertEx.Equal(entryIntent,JsonPayload.Deserialize<AlienControlIntent>(JsonPayload.Serialize(entryIntent)));
        AssertEx.Throws<ProtocolException>(()=>AlienPvPNativeCodec.Validate(frame with {Action=AlienAction.Attack}));
        AssertEx.Throws<ProtocolException>(()=>AlienPvPNativeCodec.Validate(frame with {Kind=AlienPvPNativeKind.Status}));
        var command=new GameCommandExecutePayload(1,Guid.NewGuid(),2,Guid.NewGuid(),GameCommandNames.AlienPvPVents,
            new Dictionary<string,string>{{"page","63"}});
        ModelValidation.Validate(command);
        AssertEx.Throws<ProtocolException>(()=>ModelValidation.Validate(command with {Arguments=new Dictionary<string,string>{{"page","64"}}}));
        var result=new GameCommandResultPayload(1,command.RequestId,2,command.RuntimeIdentity,
            command.Command,"ALIEN_PVP_VENTS_READY",new string('x',6000),true);
        ModelValidation.Validate(result);
        AssertEx.Throws<ProtocolException>(()=>ModelValidation.Validate(result with {Command=GameCommandNames.AlienPvPProbe}));
        return Task.CompletedTask;
    }
}
