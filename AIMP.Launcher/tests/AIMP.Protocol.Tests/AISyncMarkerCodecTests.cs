using System.Buffers.Binary;
using AIMP.Protocol;
namespace AIMP.Protocol.Tests;
internal static class AISyncMarkerCodecTests
{
    private static void Check(bool ok) { if (!ok) throw new InvalidOperationException("marker codec assertion failed"); }
    private static void Reject(Action action) { try { action(); } catch(ProtocolException) { return; } throw new InvalidOperationException("invalid marker accepted"); }
    private static uint[] Words() {
        var w=new uint[36];w[0]=24;w[1]=1;
        for(uint i=2;i<10;++i)w[i]=0x03020100+(i-2)*0x04040404;
        w[10]=1;w[12]=1;return w;
    }
    private static AISyncTransaction Record() {
        var before=new AISyncMovementParameters { X=1,Y=2,Z=3,Query0=BitConverter.UInt32BitsToSingle(0x7FC01234) };
        return new() {
            Session=new Guid("00000001-0000-0000-0000-000000000000"),AuthorityEpoch=2,WorldEpoch=3,ConsumerEpoch=4,
            EntityA=1,EntityB=2,Birth=1,CharacterClass=1,OwnerEpoch=1,Capability=AISyncTransactionCapability.Movement,
            CommandId=1,GlobalSequence=1,OwnerSequence=1,ParameterRevision=1,Opcode=AISyncTransactionOpcode.Begin,
            NativeKind=2,NativeIntent=new(Words()),Movement=new(1,1,0,AISyncMovementNavigation.HostRoute,before,before with { Retained=1 },default)
        };
    }
    internal static Task RunAllAsync() { Domain();Operations();return Task.CompletedTask; }
    private static void Domain() {
        var words=Words();var p=new AISyncNativeIntent(words);words[2]=0;Check(p.Words[2]==0x03020100);
        var bytes=AISyncNativeIntentCodec.Encode(p);Check(AISyncNativeIntentCodec.Decode(bytes)==p);
        for(var i=0;i<32;++i)Check(bytes[8+i]==i);
        Check(bytes[0]==24&&bytes[4]==1&&bytes[40]==1&&bytes[48]==1&&bytes.AsSpan(56).ToArray().All(b=>b==0));
        foreach(var at in new[]{1,10,11,12,13})foreach(var v in new uint[]{0,1,2,1000000,1000001,uint.MaxValue}) {
            var w=Words();w[at]=v;var valid=at==1?v==1:at==10?v>0&&v<=1000000:at==11?v<=1000000:v<=1;
            if(valid)_=new AISyncNativeIntent(w);else Reject(()=>_=new AISyncNativeIntent(w));
        }
        var empty=Words();Array.Clear(empty,2,8);Reject(()=>_=new AISyncNativeIntent(empty));
        for(var i=14;i<36;++i)foreach(var v in new uint[]{1,0x80000000}) {
            var w=Words();w[i]=v;Reject(()=>_=new AISyncNativeIntent(w));
            var wire=bytes.ToArray();BinaryPrimitives.WriteUInt32LittleEndian(wire.AsSpan(i*4),v);Reject(()=>AISyncNativeIntentCodec.Decode(wire));
        }
    }
    private static void Operations() {
        var r=Record();var scope=new AISyncExecutionScope(r.Session,r.AuthorityEpoch,r.WorldEpoch,r.ConsumerEpoch);
        AISyncExecutionCodec.ValidateExecutable(r);
        foreach(var op in new[]{AISyncTransactionOpcode.Begin,AISyncTransactionOpcode.Submit,AISyncTransactionOpcode.Update})
            AISyncExecutionCodec.ValidateExecutable(r with { Opcode=op });
        foreach(var kind in new uint[]{1,3,4,5,7,8,0x200})Reject(()=>AISyncExecutionCodec.ValidateExecutable(r with { NativeKind=kind }));
        Reject(()=>AISyncExecutionCodec.ValidateExecutable(r with { NativeKind=0x200,NativeAdmission=1,Movement=default }));
        var nonMarker=new uint[36];nonMarker[0]=1;
        foreach(var bad in new[]{
            r with { NativeIntent=null },r with { NativeIntent=new(nonMarker) },
            r with { Flags=AISyncTransactionFlags.HasGoal },r with { Mode=1 },r with { Gait=1 },r with { ScalarRetention=2 },
            r with { GoalX=-0F },r with { GoalY=-0F },r with { GoalZ=-0F },r with { Distance=-0F },r with { Stop=-0F },r with { Aux=-0F },
            r with { Movement=default },r with { Movement=r.Movement with { Version=2 } },r with { Movement=r.Movement with { Parts=2 } },
            r with { Movement=r.Movement with { PrimaryFlags=1 } },r with { Movement=r.Movement with { Navigation=AISyncMovementNavigation.LocalNative } },
            r with { Movement=r.Movement with { After=r.Movement.After with { X=2 } } },
            r with { Movement=r.Movement with { After=r.Movement.After with { Query0=BitConverter.UInt32BitsToSingle(0x7FC01235) } } },
            r with { Movement=r.Movement with { After=r.Movement.After with { StopDistance=-0F } } }
        }) Reject(()=>AISyncExecutionCodec.ValidateExecutable(bad));
        foreach(var op in new[]{AISyncTransactionOpcode.Cancel,AISyncTransactionOpcode.Stop,AISyncTransactionOpcode.Complete,AISyncTransactionOpcode.Release}) {
            Reject(()=>AISyncExecutionCodec.ValidateExecutable(r with { Opcode=op }));
            var t=r with { Opcode=op,NativeIntent=null,Movement=default };AISyncExecutionCodec.ValidateExecutable(t);
            var batch=AISyncExecutionCodec.EncodeBatch(new(scope,5,new[]{t}));Check(batch[4]==3&&AISyncExecutionCodec.DecodeBatch(batch).Records[0]==t);
            var receipt=new AISyncExecutionReceipt(scope,5,t,AISyncExecutionStage.Received,AISyncExecutionResult.Ok);
            var bytes=AISyncExecutionCodec.EncodeReceipt(receipt);Check(bytes[4]==3&&AISyncExecutionCodec.DecodeReceipt(bytes)==receipt);
            foreach(var version in new ushort[]{1,2}) {
                var stride=version==1?160:AISyncExecutionCodec.ExtendedRecordBytes;
                var old=bytes.AsSpan(0,64+stride+8).ToArray();BinaryPrimitives.WriteUInt16LittleEndian(old.AsSpan(4),version);
                BinaryPrimitives.WriteUInt32LittleEndian(old.AsSpan(8),(uint)old.Length);old.AsSpan(old.Length-8).Clear();old[^8]=1;
                Reject(()=>AISyncExecutionCodec.DecodeReceipt(old));
                var oldBatch=batch.AsSpan(0,72+stride).ToArray();BinaryPrimitives.WriteUInt16LittleEndian(oldBatch.AsSpan(4),version);
                BinaryPrimitives.WriteUInt32LittleEndian(oldBatch.AsSpan(8),(uint)oldBatch.Length);
                Reject(()=>AISyncExecutionCodec.DecodeBatch(oldBatch));
            }
        }
        var wire=AISyncExecutionCodec.EncodeBatch(new(scope,5,new[]{r}));
        Check(wire[4]==3&&wire.Length==72+AISyncExecutionCodec.NativeIntentRecordBytes);
        var decoded=AISyncExecutionCodec.DecodeBatch(wire).Records[0];
        Check(decoded.NativeIntent==r.NativeIntent&&AISyncMovementCodec.Encode(decoded.Movement).SequenceEqual(AISyncMovementCodec.Encode(r.Movement)));
        Check(wire.AsSpan(72+AISyncExecutionCodec.ExtendedRecordBytes).SequenceEqual(AISyncNativeIntentCodec.Encode(r.NativeIntent)));
        var expected=new AISyncExecutionReceipt(scope,5,r,AISyncExecutionStage.Received,AISyncExecutionResult.Ok);
        wire=AISyncExecutionCodec.EncodeReceipt(expected);var echo=AISyncExecutionCodec.DecodeReceipt(wire);
        Check(echo.Operation.NativeIntent==r.NativeIntent&&AISyncExecutionCodec.EncodeOperation(echo.Operation).SequenceEqual(AISyncExecutionCodec.EncodeOperation(r)));
        wire[64+AISyncExecutionCodec.ExtendedRecordBytes+8]^=1;Check(AISyncExecutionCodec.DecodeReceipt(wire).Operation.NativeIntent!=r.NativeIntent);
        wire[64+AISyncExecutionCodec.ExtendedRecordBytes+56]=1;Reject(()=>AISyncExecutionCodec.DecodeReceipt(wire));
    }
}
