namespace AIMP.Launcher;

internal sealed record VoiceSettings(bool Enabled = true, string Mode = "pushToTalk",
    string MicrophoneId = "default", int PushToTalkKey = 0x56, float ActivationThreshold = 0.02f,
    float InputGain = 1f, float PlaybackVolume = 1f)
{
    internal VoiceSettings Normalize() => this with
    {
        Mode = Mode is "pushToTalk" or "voiceActivity" ? Mode : "pushToTalk",
        MicrophoneId = string.IsNullOrWhiteSpace(MicrophoneId) || MicrophoneId.Length > 4096 || MicrophoneId.Any(char.IsControl)
            ? "default" : MicrophoneId.Trim(),
        PushToTalkKey = PushToTalkKey is >= 1 and <= 254 ? PushToTalkKey : 0x56,
        ActivationThreshold = Clamp(ActivationThreshold, 0, 1, 0.02f),
        InputGain = Clamp(InputGain, 0, 4, 1),
        PlaybackVolume = Clamp(PlaybackVolume, 0, 1, 1)
    };

    internal bool TryValidate(out string error)
    {
        error = Mode is not ("pushToTalk" or "voiceActivity") ? "Invalid voice mode."
            : string.IsNullOrWhiteSpace(MicrophoneId) || MicrophoneId.Length > 4096 || MicrophoneId.Any(char.IsControl) ? "Invalid microphone ID."
            : PushToTalkKey is < 1 or > 254 ? "Push-to-talk key must be a Windows virtual key between 1 and 254."
            : !InRange(ActivationThreshold, 0, 1) ? "Voice activation threshold must be between 0 and 1."
            : !InRange(InputGain, 0, 4) ? "Voice input gain must be between 0 and 4."
            : !InRange(PlaybackVolume, 0, 1) ? "Voice playback volume must be between 0 and 1."
            : string.Empty;
        return error.Length == 0;
    }

    private static bool InRange(float v, float min, float max) => float.IsFinite(v) && v >= min && v <= max;
    private static float Clamp(float v, float min, float max, float fallback) => float.IsFinite(v) ? Math.Clamp(v, min, max) : fallback;
}
