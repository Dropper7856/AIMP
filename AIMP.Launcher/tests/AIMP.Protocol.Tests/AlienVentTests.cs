namespace AIMP.Protocol.Tests;

internal static class AlienVentTests
{
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
        AssertEx.Throws<ProtocolException>(()=>AlienPvPNativeCodec.Validate(frame with {Action=AlienAction.EnterVent}));
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
