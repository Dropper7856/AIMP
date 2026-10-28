using System.Collections.Concurrent;
using AIMP.Launcher;
using static AIMP.Protocol.Tests.AISyncExecutionTransportTests;

namespace AIMP.Protocol.Tests;

internal static class PlayerIncarnationTests
{
    private static PlayerPositionSnapshotPayload Position(Guid session,ulong peer,int generation,ulong sequence,uint life) =>
        new(session,peer,generation,sequence,DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),1,2,3,0,PlayerMoveType.Walk,1,
            CharacterIncarnation:life);

    internal static Task AllocationAsync()
    {
        var owner = new PlayerIncarnationAllocator(2); var session = Guid.NewGuid(); var connection = Guid.NewGuid();
        var state = NativeRuntimeSnapshot.Empty with { IpcConnected=true,ProcessId=7,LevelGeneration=2,
            LocalCharacterGeneration=2,LocalCharacterSerial=40,LocalCharacterBase=0x100000,LocalCharacterRuntimeId=55,
            LocalCharacterConnectionIdentity=connection };
        uint Observe(NativeRuntimeSnapshot snapshot,Guid? pipe=null,int world=1) => owner.Observe(session,9,world,snapshot,pipe??connection);
        AssertEx.Equal(1U,Observe(state)); AssertEx.Equal(1U,Observe(state));
        // Same address and +50, distinct actual constructor.
        state=state with {LocalCharacterSerial=41}; AssertEx.Equal(2U,Observe(state));
        AssertEx.Equal(0U,Observe(state with {IpcConnected=false}));
        AssertEx.Equal(3U,Observe(state),"Retirement must not revive its old portable token.");
        AssertEx.Equal(0U,Observe(state,Guid.NewGuid()));
        var replacement=Guid.NewGuid(); state=state with {LocalCharacterConnectionIdentity=replacement};
        AssertEx.Equal(4U,Observe(state,replacement));
        AssertEx.Equal(5U,Observe(state,replacement,world:2));
        state=state with {LevelGeneration=3,LocalCharacterGeneration=3};
        AssertEx.Equal(6U,Observe(state,replacement,world:2));
        AssertEx.Equal(1U,owner.Observe(session,10,2,state,replacement));
        AssertEx.Equal(0U,owner.Observe(session,11,2,state,replacement));
        AssertEx.Equal(6U,Observe(state,replacement,world:2),"Capacity must not evict existing identity history.");
        AssertEx.Equal(0U,owner.Observe(Guid.Empty,9,2,state,replacement));
        AssertEx.Equal(0U,owner.Observe(session,0,2,state,replacement));
        return Task.CompletedTask;
    }

    internal static Task WindowAsync()
    {
        var session=Guid.NewGuid(); var window=new PlayerPositionReceiveWindow();
        var value=Position(session,9,1,1,5);
        AssertEx.Equal(value,JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(JsonPayload.Serialize(value)));
        AssertEx.True(window.TryAccept(value));
        AssertEx.True(window.TryAccept(value with {Sequence=2}));
        AssertEx.False(window.TryAccept(value with {Sequence=3,CharacterIncarnation=4}));
        AssertEx.True(window.TryAccept(value with {Sequence=3,CharacterIncarnation=0}));
        AssertEx.False(window.TryAccept(value with {Sequence=4}));
        AssertEx.True(window.TryAccept(value with {Sequence=4,CharacterIncarnation=6}));
        AssertEx.False(window.TryAccept(value with {Sequence=5,CharacterIncarnation=6,LevelGeneration=2}));
        AssertEx.True(window.TryAccept(value with {Sequence=1,CharacterIncarnation=7,LevelGeneration=2}));
        AssertEx.False(window.TryAccept(value with {Sequence=9,CharacterIncarnation=8}));
        var track=new RemotePositionTrack();
        var oldPosture=new PlayerPostureSnapshot(false,false,false,0,0,20,2,4,123,true);
        AssertEx.True(track.TryAccept(value with {Posture=oldPosture},DateTimeOffset.UtcNow));
        track.BindCharacter(0x200000);
        var next=value with {Sequence=2,CharacterIncarnation=6,Posture=oldPosture with {TraversalRevision=1,TraversalActive=false}};
        AssertEx.True(track.TryAccept(next,DateTimeOffset.UtcNow));
        AssertEx.Equal(1UL,track.Snapshot()!.Posture!.TraversalRevision);
        AssertEx.True(track.Snapshot()!.Previous is null,"Motion must not span two Character lives.");
        return Task.CompletedTask;
    }

    internal static async Task TransportAsync()
    {
        using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(15)); var token=timeout.Token;
        await using var host=Host(); await using var first=await Client(host,"Dallas",token);
        await using var second=await Client(host,"Lambert",token); var round=await Start(host,new[]{first,second},token);
        var values=new ConcurrentQueue<PlayerPositionSnapshotPayload>();
        var errors=new ConcurrentQueue<ErrorPayload>();
        second.PlayerPositionReceived+=(_,p)=>values.Enqueue(p); first.ErrorReceived+=(_,e)=>errors.Enqueue(e);
        var value=Position(host.SessionId,first.PeerId,round.LevelGeneration,1,12);
        await first.SendPlayerPositionAsync(value,token); await Wait(()=>values.Count==1,token);
        AssertEx.Equal(12U,values.Single().CharacterIncarnation);
        await first.SendPlayerPositionAsync(value with {Sequence=2,CharacterIncarnation=11},token);
        await Wait(()=>!errors.IsEmpty,token); AssertEx.Equal(1,values.Count);
        await first.SendPlayerPositionAsync(value with {Sequence=3,CharacterIncarnation=13},token);
        await Wait(()=>values.Count==2,token); AssertEx.Equal(13U,values.Last().CharacterIncarnation);
        try { await first.SendPlayerPositionAsync(value with {Sequence=4,PeerId=second.PeerId},token);
            throw new InvalidOperationException("Peer impersonation was accepted."); }
        catch(InvalidOperationException exception) when(exception.Message.Contains("identity")) { }
    }
}
