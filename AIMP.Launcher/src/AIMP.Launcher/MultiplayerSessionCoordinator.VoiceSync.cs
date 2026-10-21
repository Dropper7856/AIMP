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

    private Task HandleVoiceBridgeAsync(ScaleformBridgeMessage message) =>
        string.Equals(message.Type.Trim(), "VOICE_SAVE_SETTINGS", StringComparison.OrdinalIgnoreCase)
            ? SaveVoiceSettingsAsync(message)
            : SendVoiceSettingsAsync();

    private async Task SendVoiceSettingsAsync()
    {
        var voice = (_settingsStore.Load().Voice ?? new VoiceSettings()).Normalize();
        var values = new Dictionary<string, string>
        {
            ["enabled"] = voice.Enabled ? "1" : "0", ["mode"] = voice.Mode,
            ["key"] = voice.PushToTalkKey.ToString(CultureInfo.InvariantCulture),
            ["microphone"] = voice.MicrophoneId,
            ["threshold"] = voice.ActivationThreshold.ToString(CultureInfo.InvariantCulture)
        };
        AddMicrophoneDevices(values);
        await _bridge.SendAsync("VOICE_SETTINGS", values).ConfigureAwait(false);
    }

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

    private async Task SaveVoiceSettingsAsync(ScaleformBridgeMessage message)
    {
        var current = _settingsStore.Load();
        var voice = (current.Voice ?? new VoiceSettings()).Normalize();
        var mode = message.Get("mode", voice.Mode);
        var mic = message.Get("microphone", voice.MicrophoneId);
        var keyText = message.Get("key", voice.PushToTalkKey.ToString(CultureInfo.InvariantCulture));
        if (!int.TryParse(keyText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var key))
            throw new InvalidOperationException("Invalid push-to-talk key.");
        var next = voice with { Enabled = message.Get("enabled", voice.Enabled ? "1" : "0") == "1",
            Mode = mode, MicrophoneId = mic, PushToTalkKey = key };
        if (!next.TryValidate(out var error)) throw new InvalidOperationException(error);
        _settingsStore.Save(current with { Voice = next });
        await SendVoiceSettingsAsync().ConfigureAwait(false);
    }
}
