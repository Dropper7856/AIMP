using System.Buffers.Binary;

namespace AIMP.Protocol;

public sealed record VoiceFramePayload(
    Guid SessionId, ulong PeerId, int LevelGeneration, uint Sequence, byte[] Audio);

/// <summary>Mono IMA ADPCM. Every 20 ms block starts with its own PCM predictor and index.</summary>
public static class VoiceCodec
{
    public const int SampleRate = 16000;
    public const int SamplesPerFrame = 320;
    public const int AudioSize = 164;
    public const int NetworkSize = 32 + AudioSize;
    public const int MaximumPacketSize = 512; // Excludes the 16-byte session ID.
    private static readonly int[] Steps =
    [
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
        73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
        408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
        1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
        5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
        18500,20350,22385,24623,27086,29794,32767
    ];
    private static readonly int[] IndexChanges = [-1,-1,-1,-1,2,4,6,8];

    // Audio: i16 predictor LE, u8 step index, u8 reserved (zero), then 319
    // low-nibble-first IMA codes. The unused final high nibble must be zero.
    public static byte[] Encode(ReadOnlySpan<short> pcm)
    {
        if (pcm.Length != SamplesPerFrame)
            throw new ArgumentException("Voice requires exactly 320 PCM samples.", nameof(pcm));
        var audio = new byte[AudioSize];
        var predictor = (int)pcm[0];
        var index = 0;
        BinaryPrimitives.WriteInt16LittleEndian(audio, pcm[0]);
        for (var i = 1; i < pcm.Length; i++)
        {
            var difference = pcm[i] - predictor;
            var code = difference < 0 ? 8 : 0;
            difference = Math.Abs(difference);
            var step = Steps[index];
            if (difference >= step) { code |= 4; difference -= step; }
            if (difference >= (step >> 1)) { code |= 2; difference -= step >> 1; }
            if (difference >= (step >> 2)) code |= 1;
            Apply(code, ref predictor, ref index);
            audio[4 + (i - 1) / 2] |= (byte)(code << (((i - 1) & 1) * 4));
        }
        return audio;
    }

    public static short[] Decode(ReadOnlySpan<byte> audio)
    {
        ValidateAudio(audio);
        var pcm = new short[SamplesPerFrame];
        var predictor = (int)BinaryPrimitives.ReadInt16LittleEndian(audio);
        var index = (int)audio[2];
        pcm[0] = (short)predictor;
        for (var i = 1; i < pcm.Length; i++)
        {
            var code = (audio[4 + (i - 1) / 2] >> (((i - 1) & 1) * 4)) & 15;
            Apply(code, ref predictor, ref index);
            pcm[i] = (short)predictor;
        }
        return pcm;
    }

    public static void ValidateAudio(ReadOnlySpan<byte> audio)
    {
        if (audio.Length != AudioSize || audio[2] > 88 || audio[3] != 0
            || (audio[^1] & 0xf0) != 0)
            throw new ProtocolException("Malformed voice ADPCM block.");
    }

    public static byte[] EncodeNetwork(VoiceFramePayload frame)
    {
        Validate(frame);
        var bytes = new byte[NetworkSize];
        frame.SessionId.TryWriteBytes(bytes);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), frame.PeerId);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(24), frame.LevelGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(28), frame.Sequence);
        frame.Audio.CopyTo(bytes, 32);
        return bytes;
    }

    public static VoiceFramePayload DecodeNetwork(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != NetworkSize)
            throw new ProtocolException("Malformed voice network frame size.");
        var frame = new VoiceFramePayload(new Guid(bytes[..16]),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes[16..]),
            BinaryPrimitives.ReadInt32LittleEndian(bytes[24..]),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes[28..]), bytes[32..].ToArray());
        Validate(frame);
        return frame;
    }

    public static void Validate(VoiceFramePayload frame)
    {
        ArgumentNullException.ThrowIfNull(frame);
        if (frame.SessionId == Guid.Empty || frame.PeerId == 0 || frame.LevelGeneration < 1
            || frame.Sequence == 0 || frame.Audio is null)
            throw new ProtocolException("Invalid voice frame identity.");
        ValidateAudio(frame.Audio);
    }

    private static void Apply(int code, ref int predictor, ref int index)
    {
        var step = Steps[index];
        var delta = (step >> 3) + ((code & 4) != 0 ? step : 0)
            + ((code & 2) != 0 ? step >> 1 : 0) + ((code & 1) != 0 ? step >> 2 : 0);
        predictor = Math.Clamp(predictor + ((code & 8) != 0 ? -delta : delta), short.MinValue, short.MaxValue);
        index = Math.Clamp(index + IndexChanges[code & 7], 0, 88);
    }
}
