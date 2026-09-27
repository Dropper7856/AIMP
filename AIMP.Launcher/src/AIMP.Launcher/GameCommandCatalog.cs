using AIMP.Protocol;
using System.Globalization;
using System.Text;

namespace AIMP.Launcher;

internal enum GameCommandParameterKind
{
    Text,
    MultilineText,
    Decimal,
    Choice,
    Boolean
}

internal sealed record GameCommandChoice(string Label, string Value);

internal enum GameCommandExecutionTarget
{
    Native,
    Launcher,
    Multiplayer
}

internal static class LauncherGameCommandNames
{
    internal const string SpawnConnectedPlayers = "SPAWN_CONNECTED_PLAYERS";
    internal const string ProvisionRemoteEquipment = "PROVISION_REMOTE_EQUIPMENT";
    internal const string WorldNetProbe = "WORLD_NET_PROBE";
}

internal sealed record GameCommandParameterDefinition(
    string Key,
    string Label,
    GameCommandParameterKind Kind,
    string DefaultValue,
    string Description,
    decimal Minimum = 0,
    decimal Maximum = 0,
    decimal Increment = 1,
    int DecimalPlaces = 0,
    IReadOnlyList<GameCommandChoice>? Choices = null,
    int MaximumUtf8Bytes = 0);

internal sealed record GameCommandDefinition(
    string Name,
    string Label,
    string Description,
    IReadOnlyList<GameCommandParameterDefinition> Parameters,
    GameCommandExecutionTarget Target = GameCommandExecutionTarget.Native);

internal static class GameCommandCatalog
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static IReadOnlyList<GameCommandDefinition> Commands { get; } =
        new[]
        {
            new GameCommandDefinition(
                LauncherGameCommandNames.SpawnConnectedPlayers,
                "Spawn Remote Player Characters (network)",
                "Host-only network command. Every connected launcher spawns one local Character for each other player in the active session.",
                Array.Empty<GameCommandParameterDefinition>(),
                GameCommandExecutionTarget.Multiplayer),
            new GameCommandDefinition(
                LauncherGameCommandNames.ProvisionRemoteEquipment,
                "Give Equipment to Remote Characters (manual)",
                "Local launcher command. Select a CATHODE weapon/equipment archetype and provision it to every mapped remote Character through the native WEAPON_GiveToCharacter path. Public SPAWN/output ports are followed through nested SharedScripts; unsupported EQUIPPABLE_ITEM/wrapper contracts stay hidden or fail closed.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "equipmentId", "Equipment", GameCommandParameterKind.Choice,
                        "npc-motion-tracker",
                        "Weapon/equipment archetype to give to each remote Character.",
                        Choices: RemoteEquipmentDefinitions.Selectable
                            .Select(item => new GameCommandChoice(item.Label, item.Id))
                            .ToArray())
                },
                GameCommandExecutionTarget.Launcher),
            new GameCommandDefinition(
                LauncherGameCommandNames.WorldNetProbe,
                "World Network Probe (v12.4)",
                "Host-only no-op network diagnostic. Loads the current AIMPWM manifest, verifies every peer has the exact same MapContentId/hash, then transports one synthetic WorldEvent through host authority to each DLL. No environment gameplay is executed.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "manifestPath", "Manifest path", GameCommandParameterKind.Text,
                        string.Empty,
                        "Absolute path to the .aimpwm file produced by --compile-world-manifest.",
                        MaximumUtf8Bytes: 1024)
                },
                GameCommandExecutionTarget.Multiplayer),
            new GameCommandDefinition(
                GameCommandNames.WorldInteractionStats,
                "World CATHODE Diagnostics (detailed)",
                "Start the historical bounded CATHODE probes and state recordings. Use WorldSync 3 - Public ROOT Stats for ordinary WS3 testing and FPS comparisons.",
                Array.Empty<GameCommandParameterDefinition>()),
            new GameCommandDefinition(
                GameCommandNames.WorldCompositeInputStats,
                "WorldSync 3 - Public ROOT Stats",
                "Read public ROOT sends, retail applies, descendant suppression and rejection counters. Use before and after ProbeDirect, ProbeFanout and ProbeNested. No scan or historical probe is started.",
                Array.Empty<GameCommandParameterDefinition>()),
            new GameCommandDefinition(
                GameCommandNames.WorldStateDiscoveryBaseline,
                "World State Discovery - Baseline (Fix3.28)",
                "Start a fresh local-only baseline capture of the active CATHODE runtime state. Uses the bounded incremental state scanner only; no network operation, gameplay write, historical probe, or replay is armed. Wait for the in-game completion popup before triggering the witness action.",
                Array.Empty<GameCommandParameterDefinition>()),
            new GameCommandDefinition(
                GameCommandNames.WorldStateDiscoveryAfter,
                "World State Discovery - After / Diff (Fix3.28)",
                "Capture the post-action CATHODE runtime state and compute a normalized differential against the Fix3.28 baseline. Run only after the baseline completion popup and after exactly one witness action.",
                Array.Empty<GameCommandParameterDefinition>()),
            new GameCommandDefinition(
                GameCommandNames.WorldStateDiscoveryStatus,
                "World State Discovery - Status (Fix3.28)",
                "Read the Fix3.28 state-discovery phase and counters without starting a capture or arming any historical WorldSync probe.",
                Array.Empty<GameCommandParameterDefinition>()),
            new GameCommandDefinition(
                GameCommandNames.WorldRegistryProbe,
                "World Registry Probe (v12.3)",
                "Load an AIMPWM05 manifest runtime index and resolve relevant World entities through the native CATHODE manager on the validated engine thread. This is an explicit one-shot diagnostic; it performs no frame polling.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "manifestPath", "Manifest path", GameCommandParameterKind.Text,
                        string.Empty,
                        "Absolute path to the .aimpwm file produced by --compile-world-manifest.",
                        MaximumUtf8Bytes: 1024)
                }),
            new GameCommandDefinition(
                GameCommandNames.PopupMessage,
                "Popup Message",
                "Display a message through the game's native popup manager.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "headerText", "Header", GameCommandParameterKind.Text, "AIMP",
                        "Header text, limited to 63 UTF-8 bytes.", MaximumUtf8Bytes: 63),
                    new GameCommandParameterDefinition(
                        "mainText", "Message", GameCommandParameterKind.MultilineText,
                        "Command sent from the launcher.",
                        "Message text, limited to 63 UTF-8 bytes.", MaximumUtf8Bytes: 63),
                    new GameCommandParameterDefinition(
                        "duration", "Duration (seconds)", GameCommandParameterKind.Decimal, "5",
                        "Display duration from 0.5 to 120 seconds.", 0.5M, 120M, 0.5M, 1),
                    new GameCommandParameterDefinition(
                        "icon", "Icon", GameCommandParameterKind.Choice,
                        PopupMessageIcons.Alert, "Popup icon.", Choices: new[]
                        {
                            new GameCommandChoice("Alert", PopupMessageIcons.Alert),
                            new GameCommandChoice("Audio log", PopupMessageIcons.Audiolog),
                            new GameCommandChoice("None", PopupMessageIcons.None)
                        }),
                    new GameCommandParameterDefinition(
                        "soundEvent", "Sound", GameCommandParameterKind.Choice,
                        PopupMessageSoundEvents.ObjectiveNew,
                        "Native sound event.", Choices: new[]
                        {
                            new GameCommandChoice("OBJECTIVE_NEW",
                                PopupMessageSoundEvents.ObjectiveNew)
                        })
                }),
            new GameCommandDefinition(
                GameCommandNames.SpawnCharacter,
                "Spawn Character (experimental)",
                "Spawn a CIVILIAN character at an absolute world position.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "x", "Position X", GameCommandParameterKind.Decimal, "-49.270",
                        "Absolute X coordinate.", -100000M, 100000M, 0.1M, 3),
                    new GameCommandParameterDefinition(
                        "y", "Position Y", GameCommandParameterKind.Decimal, "19.258",
                        "Absolute Y coordinate.", -100000M, 100000M, 0.1M, 3),
                    new GameCommandParameterDefinition(
                        "z", "Position Z", GameCommandParameterKind.Decimal, "-17.890",
                        "Absolute Z coordinate.", -100000M, 100000M, 0.1M, 3)
                }),
            new GameCommandDefinition(
                GameCommandNames.CharacterDirectMove,
                "Character Direct Move (experimental)",
                "Acquire the existing NPC locomotion cycle once, then override the CIVILIAN AI controls after native preprocessing while preserving the original animation timeline.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "characterAddress", "CharacterBase address",
                        GameCommandParameterKind.Text, "0x00000000",
                        "Current 32-bit CharacterBase address from x32dbg (for example 0x2BD73220).",
                        MaximumUtf8Bytes: 10),
                    new GameCommandParameterDefinition(
                        "directionX", "Direction X (local)",
                        GameCommandParameterKind.Decimal, "0.000",
                        "X component of the native locomotion-space vector. Use the sign to choose left/right.",
                        -2M, 2M, 0.01M, 3),
                    new GameCommandParameterDefinition(
                        "directionZ", "Direction Z (local)",
                        GameCommandParameterKind.Decimal, "1.151",
                        "Z component of the native locomotion-space vector. Use the sign to choose forward/backward.",
                        -2M, 2M, 0.01M, 3),
                    new GameCommandParameterDefinition(
                        "intensity", "Native intensity",
                        GameCommandParameterKind.Decimal, "1.100",
                        "Second argument of the native locomotion call. The engine clamps it to at least 1.0.",
                        1M, 4M, 0.1M, 3),
                    new GameCommandParameterDefinition(
                        "frameCount", "Frame count",
                        GameCommandParameterKind.Decimal, "180",
                        "Manual safety duration in matching ActNPCLocomotionCycle updates. Use 0 to stop.",
                        0M, 3600M, 1M, 0)
                }),
            new GameCommandDefinition(
                GameCommandNames.CharacterActionTest,
                "Character Actions (experimental)",
                "Persistent Character input (experimental). Aim and Primary are applied at the natural equipment update. Primary requires Aim START. A START requested while Releasing is deferred until two coherent released updates have completed. No weapon-specific behavior or ShootingRequest is used. Stop Primary/Aim before changing the target or closing the panel.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "characterAddress", "CharacterBase address",
                        GameCommandParameterKind.Text, "0x00000000",
                        "Current 32-bit CharacterBase address. The launcher fills this from the tracked remote Character when available.",
                        MaximumUtf8Bytes: 10),
                    new GameCommandParameterDefinition(
                        "action", "Action", GameCommandParameterKind.Choice,
                        "aim-start",
                        "Publish an AIMP input intention. Accepted does not mean a shot has completed; runtime phase changes are reported in native.log.",
                        Choices: new[]
                        {
                            new GameCommandChoice("Aim START", "aim-start"),
                            new GameCommandChoice("Aim STOP", "aim-stop"),
                            new GameCommandChoice("Primary START", "primary-start"),
                            new GameCommandChoice("Primary STOP", "primary-stop")
                        })
                }),
            new GameCommandDefinition(
                GameCommandNames.CharacterAmmoRefill,
                "Remplir les munitions de test",
                "Manual test-only refill of the current AIMP Character equipment to its native capacity. Stop Primary and wait for Idle first. Aim may remain active. Does not animate Reload, alter capacity, or refill automatically.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "characterAddress", "CharacterBase address",
                        GameCommandParameterKind.Text, "0x00000000",
                        "Current tracked AIMP-owned CIVILIAN. Only this Character's current equipment is affected.",
                        MaximumUtf8Bytes: 10)
                }),
            new GameCommandDefinition(
                GameCommandNames.CharacterDirectStep,
                "Character Hybrid Step (experimental)",
                "Diagnostic-only physical Character step combined with a native NPC locomotion request. Automatic player replication does not use this command.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "characterAddress", "CharacterBase address",
                        GameCommandParameterKind.Text, "0x00000000",
                        "Current 32-bit CharacterBase address from x32dbg (for example 0x2C0814F0).",
                        MaximumUtf8Bytes: 10),
                    new GameCommandParameterDefinition(
                        "deltaX", "Step / direction X", GameCommandParameterKind.Decimal, "-0.010",
                        "Per-update physical X delta written to Character+0x1010 and X component submitted to the native NPC locomotion request.",
                        -1M, 1M, 0.01M, 3),
                    new GameCommandParameterDefinition(
                        "deltaY", "Step / direction Y", GameCommandParameterKind.Decimal, "0.000",
                        "Per-update physical Y delta written to Character+0x1014. Keep 0 for normal horizontal replication.",
                        -1M, 1M, 0.01M, 3),
                    new GameCommandParameterDefinition(
                        "deltaZ", "Step / direction Z", GameCommandParameterKind.Decimal, "0.010",
                        "Per-update physical Z delta written to Character+0x1018 and Z component submitted to the native NPC locomotion request.",
                        -1M, 1M, 0.01M, 3),
                    new GameCommandParameterDefinition(
                        "deltaYaw", "Yaw step",
                        GameCommandParameterKind.Decimal, "0.000",
                        "Per-update yaw delta written to Character+0x1020. XYZ continues to drive the native facing request.",
                        -1M, 1M, 0.01M, 3),
                    new GameCommandParameterDefinition(
                        "frameCount", "Frame count",
                        GameCommandParameterKind.Decimal, "180",
                        "Number of matching ActNPCLocomotionCycle updates to apply. Use 0 to stop and clear the active physical/native request.",
                        0M, 600M, 1M, 0)
                }),
            new GameCommandDefinition(
                GameCommandNames.SpawnNetZCharacter,
                "Spawn NetZ Character (safe diagnostic)",
                "Spawn a CIVILIAN character safely. Persistent NetZ participant registration remains blocked until the auxiliary resolver is restored.",
                new[]
                {
                    new GameCommandParameterDefinition(
                        "x", "Position X", GameCommandParameterKind.Decimal, "-49.270",
                        "Absolute X coordinate.", -100000M, 100000M, 0.1M, 3),
                    new GameCommandParameterDefinition(
                        "y", "Position Y", GameCommandParameterKind.Decimal, "19.258",
                        "Absolute Y coordinate.", -100000M, 100000M, 0.1M, 3),
                    new GameCommandParameterDefinition(
                        "z", "Position Z", GameCommandParameterKind.Decimal, "-17.890",
                        "Absolute Z coordinate.", -100000M, 100000M, 0.1M, 3),
                    new GameCommandParameterDefinition(
                        "participantKey", "Participant key", GameCommandParameterKind.Decimal, "1",
                        "Unique native participant lookup key.", 1M, 2147483647M, 1M, 0),
                    new GameCommandParameterDefinition(
                        "displayName", "Player name", GameCommandParameterKind.Text,
                        "RemotePlayer", "Remote participant name, limited to 63 UTF-8 bytes.",
                        MaximumUtf8Bytes: 63)
                })
        };

    internal static IReadOnlyDictionary<string, string> ValidateAndNormalize(
        GameCommandDefinition command,
        IReadOnlyDictionary<string, string> values)
    {
        ArgumentNullException.ThrowIfNull(command);
        ArgumentNullException.ThrowIfNull(values);
        var normalized = command.Parameters.ToDictionary(
            parameter => parameter.Key,
            parameter => Normalize(parameter, ReadRequired(values, parameter)),
            StringComparer.Ordinal);
        ValidatePopup(command, normalized);
        ValidateCharacterDirectMove(command, normalized);
        ValidateCharacterActionTest(command, normalized);
        ValidateCharacterDirectStep(command, normalized);
        ValidateNetZSpawn(command, normalized);
        return normalized;
    }

    private static string ReadRequired(
        IReadOnlyDictionary<string, string> values,
        GameCommandParameterDefinition parameter)
    {
        if (values.TryGetValue(parameter.Key, out var value)) return value;
        throw new InvalidOperationException($"{parameter.Label} is required.");
    }

    private static string Normalize(GameCommandParameterDefinition parameter, string value) =>
        parameter.Kind switch
        {
            GameCommandParameterKind.Text => NormalizeText(parameter, value),
            GameCommandParameterKind.MultilineText => NormalizeText(parameter, value),
            GameCommandParameterKind.Decimal => NormalizeDecimal(parameter, value),
            GameCommandParameterKind.Choice => NormalizeChoice(parameter, value),
            GameCommandParameterKind.Boolean => NormalizeBoolean(parameter, value),
            _ => throw new InvalidOperationException("Unknown command parameter type.")
        };

    private static string NormalizeText(
        GameCommandParameterDefinition parameter,
        string value)
    {
        int count;
        try { count = StrictUtf8.GetByteCount(value); }
        catch (EncoderFallbackException)
        {
            throw new InvalidOperationException($"{parameter.Label} contains invalid UTF-16.");
        }
        if (parameter.MaximumUtf8Bytes > 0 && count > parameter.MaximumUtf8Bytes)
            throw new InvalidOperationException(
                $"{parameter.Label} exceeds {parameter.MaximumUtf8Bytes} UTF-8 bytes.");
        if (value.EnumerateRunes().Any(IsInvalidControl))
            throw new InvalidOperationException($"{parameter.Label} contains a control character.");
        return value;
    }

    private static string NormalizeDecimal(
        GameCommandParameterDefinition parameter,
        string value)
    {
        var valid = decimal.TryParse(value, NumberStyles.Float,
            CultureInfo.InvariantCulture, out var number);
        if (!valid || number < parameter.Minimum || number > parameter.Maximum)
            throw new InvalidOperationException(
                $"{parameter.Label} must be between {parameter.Minimum} and {parameter.Maximum}.");
        return number.ToString($"F{parameter.DecimalPlaces}", CultureInfo.InvariantCulture);
    }

    private static string NormalizeChoice(
        GameCommandParameterDefinition parameter,
        string value)
    {
        var valid = parameter.Choices?.Any(choice =>
            string.Equals(choice.Value, value, StringComparison.Ordinal)) == true;
        return valid ? value : throw new InvalidOperationException(
            $"{parameter.Label} has an invalid value.");
    }

    private static string NormalizeBoolean(
        GameCommandParameterDefinition parameter,
        string value) => value switch
        {
            "0" => "0",
            "1" => "1",
            _ => throw new InvalidOperationException(
                $"{parameter.Label} must be checked or unchecked.")
        };

    private static void ValidatePopup(
        GameCommandDefinition command,
        IReadOnlyDictionary<string, string> values)
    {
        if (!string.Equals(command.Name, GameCommandNames.PopupMessage,
                StringComparison.Ordinal)) return;
        if (values["headerText"].Length == 0 && values["mainText"].Length == 0)
            throw new InvalidOperationException("Header and message cannot both be empty.");
    }

    private static void ValidateCharacterDirectMove(
        GameCommandDefinition command,
        IReadOnlyDictionary<string, string> values)
    {
        if (!string.Equals(command.Name, GameCommandNames.CharacterDirectMove,
                StringComparison.Ordinal)) return;
        ValidateCharacterAddress(values["characterAddress"]);
        var directionX = decimal.Parse(values["directionX"], CultureInfo.InvariantCulture);
        var directionZ = decimal.Parse(values["directionZ"], CultureInfo.InvariantCulture);
        ValidateDirectionVector(values["frameCount"], directionX, directionZ);
    }

    private static void ValidateDirectionVector(
        string frameCount,
        decimal directionX,
        decimal directionZ)
    {
        if (frameCount != "0" && directionX == 0M && directionZ == 0M)
        {
            throw new InvalidOperationException(
                "Direction X and Direction Z cannot both be zero unless Frame count is 0.");
        }
        if (directionX * directionX + directionZ * directionZ > 4M)
        {
            throw new InvalidOperationException(
                "The X/Z direction vector magnitude cannot exceed 2.0.");
        }
    }

    private static void ValidateCharacterActionTest(
        GameCommandDefinition command,
        IReadOnlyDictionary<string, string> values)
    {
        if (!string.Equals(command.Name, GameCommandNames.CharacterActionTest,
                StringComparison.Ordinal) &&
            !string.Equals(command.Name, GameCommandNames.CharacterAmmoRefill,
                StringComparison.Ordinal)) return;
        ValidateCharacterAddress(values["characterAddress"]);
    }

    private static void ValidateCharacterDirectStep(
        GameCommandDefinition command,
        IReadOnlyDictionary<string, string> values)
    {
        if (!string.Equals(command.Name, GameCommandNames.CharacterDirectStep,
                StringComparison.Ordinal)) return;
        ValidateCharacterAddress(values["characterAddress"]);
    }

    private static void ValidateCharacterAddress(string value)
    {
        var text = value;
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            text = text[2..];
        if (!uint.TryParse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture,
                out var address)
            || address < 0x10000U
            || (address & 0x0FU) != 0U)
        {
            throw new InvalidOperationException(
                "CharacterBase address must be a non-zero 16-byte-aligned 32-bit hexadecimal address.");
        }
    }

    private static void ValidateNetZSpawn(
        GameCommandDefinition command,
        IReadOnlyDictionary<string, string> values)
    {
        if (!string.Equals(command.Name, GameCommandNames.SpawnNetZCharacter,
                StringComparison.Ordinal)) return;
        if (string.IsNullOrWhiteSpace(values["displayName"]))
            throw new InvalidOperationException("Player name cannot be empty.");
    }

    private static bool IsInvalidControl(Rune rune) =>
        rune.Value == 0
        || (rune.Value < 0x20 && rune.Value is not ('\r' or '\n' or '\t'))
        || rune.Value == 0x7F;
}
