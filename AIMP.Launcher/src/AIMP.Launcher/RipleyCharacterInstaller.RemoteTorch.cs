using System.Numerics;
using CATHODE;
using CATHODE.Scripting;
using CathodeLib.ObjectExtensions;

namespace AIMP.Launcher;

internal static partial class RipleyCharacterInstaller
{
    internal const string RemoteTorchComposite = @"AIMP\Remote_Torch_Light_v2";
    internal static readonly ShortGuid RemoteTorchCompositeId = ShortGuidUtils.Generate(RemoteTorchComposite);
    private const string PlayerTorchComposite = @"Required_Assets\Player_Torch_Light";
    // The resource-backed PC spotlight in the retail torch. The parallel light
    // without a resource is a console-platform alternative, not a template.
    private static readonly ShortGuid PlayerTorchBeamId = new("52-BD-FB-F6");

    internal static bool EnsureRemoteTorch(Commands commands)
    {
        var source = commands.GetComposite(PlayerTorchComposite);
        if (source == null) return false;
        var named = commands.GetComposite(RemoteTorchComposite);
        var existing = commands.GetComposite(RemoteTorchCompositeId);
        if (!ReferenceEquals(named, existing))
            throw new InvalidDataException("Remote torch composite name/identity conflict.");
        if (source.GetEntityByID(PlayerTorchBeamId) is not FunctionEntity beam
            || !beam.function.IsFunctionType || beam.function.AsFunctionType != FunctionType.LightReference
            || beam.resources.Count == 0)
            throw new InvalidDataException("The retail resource-backed torch spotlight is missing.");
        var intensity = NominalTorchIntensity(source, beam);
        if (existing != null)
        {
            // v389 copied the fallback (1000) after removing the binding which
            // actually supplies normal torch intensity (1). Upgrade that known
            // graph in place; reject unrelated graph corruption as before.
            ValidateRemoteTorch(existing, intensity, allowLegacyIntensity: true);
            var current = (cFloat)existing.functions[0].GetParameter("intensity_multiplier").content;
            if (current.value == intensity) return false;
            existing.functions[0].AddParameter("intensity_multiplier", new cFloat(intensity), ParameterVariant.PARAMETER, true);
            ValidateRemoteTorch(existing, intensity);
            return true;
        }

        var remote = new Composite(RemoteTorchComposite) { shortGUID = RemoteTorchCompositeId };
        var light = remote.AddFunction(beam.Copy());
        // Resource table references belong to the level. Deep-copying those
        // objects would leave the new graph referring to entries absent from
        // REDS.BIN, just as for the existing remote flamethrower effects.
        for (var i = 0; i < beam.resources.Count; ++i)
        {
            light.resources[i].RenderableInstance = beam.resources[i].RenderableInstance;
            light.resources[i].AnimatedModel = beam.resources[i].AnimatedModel;
            light.resources[i].CollisionMapping = beam.resources[i].CollisionMapping;
        }
        // Only the resource-backed light is copied. The original graph's input,
        // battery, THE_PLAYER, TorchDynamicMovement, sound and animation logic
        // must never execute for another player's representation.
        light.childLinks.Clear();
        // A linked CATHODE property overrides its serialized fallback. Preserve
        // the stock beam's full-on photometry, not the unused 1000 fallback.
        light.AddParameter("intensity_multiplier", new cFloat(intensity), ParameterVariant.PARAMETER, true);
        foreach (var resource in light.resources) { resource.position = Vector3.Zero; resource.rotation = Vector3.Zero; }
        light.AddParameter("position", new cTransform(Vector3.Zero, Vector3.Zero), ParameterVariant.PARAMETER, true);
        foreach (var parameter in new[] { "is_flash_light", "attach_on_reset" })
            light.AddParameter(parameter, new cBool(false), ParameterVariant.PARAMETER, true);
        // Instantiated only on an enabled sample. Let native reset create and
        // configure the renderer light (631A70), then PlayerTorchRuntime controls
        // its visibility directly. Starting both reset gates false prevents us
        // from treating successful graph activation as successful illumination.
        foreach (var parameter in new[] { "show_on_reset", "light_on_reset", "on" })
            light.AddParameter(parameter, new cBool(true), ParameterVariant.PARAMETER, true);
        ValidateRemoteTorch(remote, intensity);
        commands.Entries.Add(remote);
        return true;
    }

    private static float NominalTorchIntensity(Composite source, FunctionEntity beam)
    {
        // Resolve only the pure float expression used by the retail beam.
        // Modulation envelopes are evaluated at their fully switched-on value;
        // never copy local battery, input or camera controllers into this graph.
        float Read(FunctionEntity entity, string parameter, int depth)
        {
            if (depth > 24) throw new InvalidDataException("Cyclic or excessive torch intensity expression.");
            var links = entity.childLinks.Where(l => l.thisParamID == ShortGuidUtils.Generate(parameter)).ToArray();
            if (links.Length == 0 && entity.GetParameter(parameter)?.content is cFloat constant)
                return constant.value;
            if (links.Length != 1 || links[0].linkedParamID != ShortGuidUtils.Generate("reference")
                || source.GetEntityByID(links[0].linkedEntityID) is not FunctionEntity input)
                throw new InvalidDataException("Unsupported retail torch intensity binding.");
            return input.function.AsFunctionType switch
            {
                FunctionType.FloatMultiply => Read(input, "LHS", depth + 1) * Read(input, "RHS", depth + 1),
                FunctionType.FloatAdd => Read(input, "LHS", depth + 1) + Read(input, "RHS", depth + 1),
                FunctionType.VariableFloat => Read(input, "initial_value", depth + 1),
                FunctionType.FloatModulateRandom => 1F,
                _ => throw new InvalidDataException("Unsupported retail torch intensity expression.")
            };
        }
        var result = Read(beam, "intensity_multiplier", 0);
        if (!float.IsFinite(result) || result <= 0)
            throw new InvalidDataException("Invalid retail torch nominal intensity.");
        return result;
    }

    private static void ValidateRemoteTorch(Composite remote, float intensity, bool allowLegacyIntensity = false)
    {
        if (remote.functions.Count != 1 || remote.GetEntities().Count() != 1
            || remote.functions[0].function.AsFunctionType != FunctionType.LightReference
            || remote.functions[0].resources.Count == 0)
            throw new InvalidDataException("Remote torch must contain only one resource-backed light.");
        var light = remote.functions[0];
        if (light.GetParameter("intensity_multiplier")?.content is not cFloat power
            || (power.value != intensity && !(allowLegacyIntensity && power.value == 1000F)))
            throw new InvalidDataException("Remote torch intensity must match the retail full-on expression.");
        foreach (var name in new[] { "is_flash_light", "attach_on_reset" })
            if (light.GetParameter(name)?.content is not cBool { value: false })
                throw new InvalidDataException($"Remote torch {name} must be disabled.");
        foreach (var name in new[] { "show_on_reset", "light_on_reset", "on" })
            if (light.GetParameter(name)?.content is not cBool { value: true })
                throw new InvalidDataException($"Remote torch {name} must initialize the renderer light.");
        if (light.childLinks.Count != 0 || remote.variables.Count != 0)
            throw new InvalidDataException("Remote torch must not schedule public inputs or control the local player.");
    }
}
