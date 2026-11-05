using System.Globalization;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private void UpdateVoiceSession(NativeRuntimeSnapshot runtime)
    {
        lock (_stateGate)
        {
            var active = _start is not null && _localGameplayReady && _sessionReleaseAuthorized
                && _roundEndingGeneration != _start.LevelGeneration && runtime.IpcConnected;
            _voiceSync.Update(new VoiceSessionContext(_start?.SessionId ?? Guid.Empty, LocalPeerId(),
                _start?.LevelGeneration ?? 0, runtime.LevelGeneration, runtime.ProcessId, active,
                active && _deathReportedGeneration != _start?.LevelGeneration, _host, _client));
        }
    }

    private void SuspendVoiceSession() => _voiceSync.Update(
        new VoiceSessionContext(Guid.Empty, 0, 0, 0, 0, false, false, null, null));

    private Task HandleVoiceBridgeAsync(ScaleformBridgeMessage message, Func<string, IReadOnlyDictionary<string, string>?, Task<bool>>? reply = null) =>
        message.Type.Trim().ToUpperInvariant() switch
        {
            "DEVELOPER_MODE" => SetDeveloperModeAsync(message, reply),
            "VOICE_SAVE_SETTINGS" => SaveVoiceSettingsAsync(message, reply),
            _ => SendVoiceSettingsAsync(reply)
        };

    private static bool IsVoiceSettingsMessage(ScaleformBridgeMessage message) =>
        message.Type.Trim().ToUpperInvariant() is "VOICE_GET_SETTINGS" or "VOICE_SAVE_SETTINGS" or "DEVELOPER_MODE";

    // The in-game pause menu has its own event-channel socket: answer on it, not
    // on the command channel, which belongs to the gameplay overlay.
    private async Task HandlePauseVoiceMessageAsync(ScaleformBridgeEvent value)
    {
        try { await HandleVoiceBridgeAsync(value.Message, value.Reply).ConfigureAwait(false); }
        catch (Exception exception) { _diagnostics.Log("VoiceSync", $"Pause menu voice settings failed: {exception.Message}"); }
    }

    private async Task SendVoiceSettingsAsync(Func<string, IReadOnlyDictionary<string, string>?, Task<bool>>? reply = null)
    {
        var settings = _settingsStore.Load();
        var voice = (settings.Voice ?? new VoiceSettings()).Normalize();
        var values = new Dictionary<string, string>
        {
            ["developerModeAvailable"] = _settingsStore.DeveloperModeAvailable ? "1" : "0",
            ["developerMode"] = settings.DeveloperMode ? "1" : "0",
            ["enabled"] = voice.Enabled ? "1" : "0", ["mode"] = voice.Mode,
            ["key"] = voice.PushToTalkKey.ToString(CultureInfo.InvariantCulture),
            ["microphone"] = voice.MicrophoneId,
            ["threshold"] = voice.ActivationThreshold.ToString(CultureInfo.InvariantCulture),
            ["gain"] = voice.InputGain.ToString(CultureInfo.InvariantCulture),
            ["volume"] = voice.PlaybackVolume.ToString(CultureInfo.InvariantCulture)
        };
        AddMicrophoneDevices(values);
        if (reply is not null) await reply("VOICE_SETTINGS", values).ConfigureAwait(false);
        else await _bridge.SendAsync("VOICE_SETTINGS", values).ConfigureAwait(false);
    }

    private static float ReadVoiceLevel(ScaleformBridgeMessage message, string key, float fallback) =>
        float.TryParse(message.Get(key, string.Empty), NumberStyles.Float, CultureInfo.InvariantCulture, out var value)
            ? value : fallback;

    private static void AddMicrophoneDevices(Dictionary<string, string> values)
    {
        var devices = VoiceMicrophoneDevices.Enumerate();
        values["count"] = devices.Count.ToString(CultureInfo.InvariantCulture);
        for (var i = 0; i < devices.Count; i++)
        {
            values[$"device{i}Id"] = devices[i].Id;
            values[$"device{i}Name"] = devices[i].Name;
        }
    }

    private async Task SaveVoiceSettingsAsync(ScaleformBridgeMessage message, Func<string, IReadOnlyDictionary<string, string>?, Task<bool>>? reply = null)
    {
        var current = _settingsStore.Load();
        var voice = (current.Voice ?? new VoiceSettings()).Normalize();
        var keyText = message.Get("key", voice.PushToTalkKey.ToString(CultureInfo.InvariantCulture));
        if (!int.TryParse(keyText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var key))
            throw new InvalidOperationException("Invalid push-to-talk key.");
        var next = voice with { Enabled = message.Get("enabled", voice.Enabled ? "1" : "0") == "1",
            Mode = message.Get("mode", voice.Mode), MicrophoneId = message.Get("microphone", voice.MicrophoneId),
            PushToTalkKey = key, ActivationThreshold = ReadVoiceLevel(message, "threshold", voice.ActivationThreshold),
            InputGain = ReadVoiceLevel(message, "gain", voice.InputGain),
            PlaybackVolume = ReadVoiceLevel(message, "volume", voice.PlaybackVolume) };
        if (!next.TryValidate(out var error)) throw new InvalidOperationException(error);
        _settingsStore.Save(current with { Voice = next });
        await SendVoiceSettingsAsync(reply).ConfigureAwait(false);
    }
}
