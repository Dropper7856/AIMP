using System.Numerics;
using System.Reflection;
using System.Text.Json;
using AIMP.Launcher;
using CATHODE;
using CATHODE.Scripting;
using CathodeLib;

namespace AIMP.Protocol.Tests;

internal static class PlayerTorchGraphTests
{
    internal static Task IsolationAsync()
    {
        using var files = new TemporaryDirectory();
        var renderables = new RenderableElements(files.Combine("REDS.BIN"), null!, null!);
        var animation = new EnvironmentAnimations(files.Combine("ENVIRONMENT_ANIMATION.DAT"), null!);
        var collision = new CollisionMaps(files.Combine("COLLISION.MAP"), null!, null!);
        var commands = new Commands(files.Combine("original.PAK"), animation, collision, renderables);
        var player = new Composite(@"Required_Assets\Player_Torch_Light");
        var beam = player.AddFunction(new FunctionEntity(new ShortGuid("52-BD-FB-F6"), FunctionType.LightReference));
        var originalPosition = new Vector3(0.165042F, 0, 0);
        beam.AddParameter("position", new cTransform(originalPosition, Vector3.Zero), ParameterVariant.PARAMETER, true);
        beam.AddParameter("is_flash_light", new cBool(true), ParameterVariant.PARAMETER, true);
        beam.AddParameter("intensity_multiplier", new cFloat(1000), ParameterVariant.PARAMETER, true);
        beam.AddParameter("outer_cone_angle", new cFloat(36), ParameterVariant.PARAMETER, true);
        beam.AddParameter("end_attenuation", new cFloat(14), ParameterVariant.PARAMETER, true);
        beam.AddParameter("gobo_texture", new cString("retail-torch-gobo.dds"), ParameterVariant.PARAMETER, true);
        var power = player.AddFunction(FunctionType.FloatMultiply);
        power.AddParameter("LHS", new cFloat(0.5F), ParameterVariant.PARAMETER, true);
        power.AddParameter("RHS", new cFloat(2F), ParameterVariant.PARAMETER, true);
        beam.AddParameterLink("intensity_multiplier", power, "reference");
        var drawable = new RenderableElements.Element();
        renderables.Entries.Add(drawable);
        beam.resources.Add(new ResourceReference { resource_id = beam.shortGUID,
            resource_type = ResourceType.RENDERABLE_INSTANCE, position = originalPosition,
            RenderableInstance = new List<RenderableElements.Element> { drawable } });
        foreach (var type in new[] { FunctionType.VariableThePlayer, FunctionType.MonitorActionMap,
            FunctionType.PlayerTorch, FunctionType.Torch_Control, FunctionType.TorchDynamicMovement })
            player.AddFunction(type);
        var input = player.AddVariable("show", DataType.FLOAT);
        input.AddParameterLink("show", beam, "show");
        commands.Entries.Add(player);
        var scene = new Composite("TorchTestScene");
        var global = new Composite("Global");
        var pause = new Composite("PauseMenu");
        commands.Entries.AddRange(new[] { scene, global, pause });
        commands.SetRootComposite(scene);
        AssertEx.True(RipleyCharacterInstaller.EnsureRemoteTorch(commands));
        AssertEx.False(RipleyCharacterInstaller.EnsureRemoteTorch(commands));
        var remote = commands.GetComposite(RipleyCharacterInstaller.RemoteTorchCompositeId)!;
        AssertRemote(remote);
        AssertEx.Equal(7, player.functions.Count);
        AssertEx.True(((cBool)beam.GetParameter("is_flash_light").content).value);
        AssertEx.Equal(originalPosition, beam.resources[0].position);
        AssertEx.True(ReferenceEquals(beam.resources[0].RenderableInstance, remote.functions[0].resources[0].RenderableInstance));
        AssertEx.Equal(1000F, ((cFloat)beam.GetParameter("intensity_multiplier").content).value);
        AssertOptics(beam, remote.functions[0]);
        remote.functions[0].AddParameter("intensity_multiplier", new cFloat(1000), ParameterVariant.PARAMETER, true);
        AssertEx.True(RipleyCharacterInstaller.EnsureRemoteTorch(commands));
        AssertEx.False(RipleyCharacterInstaller.EnsureRemoteTorch(commands));
        AssertRemote(remote);

        remote.functions[0].AddParameter("is_flash_light", new cBool(true), ParameterVariant.PARAMETER, true);
        AssertEx.Throws<InvalidDataException>(() => RipleyCharacterInstaller.EnsureRemoteTorch(commands));
        return Task.CompletedTask;
    }

    private static void AssertRemote(Composite remote)
    {
        AssertEx.Equal(1, remote.functions.Count);
        AssertEx.Equal(1, remote.GetEntities().Count());
        var light = remote.functions.Single();
        AssertEx.Equal(FunctionType.LightReference, light.function.AsFunctionType);
        AssertEx.Equal(0, light.childLinks.Count);
        AssertEx.Equal(1F, ((cFloat)light.GetParameter("intensity_multiplier").content).value);
        AssertEx.Equal(Vector3.Zero, ((cTransform)light.GetParameter("position").content).position);
        AssertEx.Equal(Vector3.Zero, light.resources[0].position);
        foreach (var name in new[] { "is_flash_light", "attach_on_reset" })
            AssertEx.False(((cBool)light.GetParameter(name).content).value);
        foreach (var name in new[] { "show_on_reset", "light_on_reset", "on" })
            AssertEx.True(((cBool)light.GetParameter(name).content).value);
        AssertEx.Equal(0, remote.variables.Count);
    }

    private static void AssertOptics(FunctionEntity original, FunctionEntity remote)
    {
        var intentionallyChanged = new[] { "position", "intensity_multiplier", "is_flash_light", "attach_on_reset",
            "show_on_reset", "light_on_reset", "on" }.Select(name => ShortGuidUtils.Generate(name)).ToHashSet();
        var options = new JsonSerializerOptions { IncludeFields = true };
        foreach (var parameter in original.parameters.Where(p => !intentionallyChanged.Contains(p.name)))
        {
            var copied = remote.parameters.Single(p => p.name == parameter.name);
            AssertEx.Equal(JsonSerializer.Serialize(parameter.content, parameter.content.GetType(), options),
                JsonSerializer.Serialize(copied.content, copied.content.GetType(), options));
        }
    }

    internal static Task RetailAsync()
    {
        var path = Environment.GetEnvironmentVariable("AIMP_TEST_COMMANDS");
        if (string.IsNullOrWhiteSpace(path))
        {
            Console.WriteLine("SKIP retail torch graph: AIMP_TEST_COMMANDS is not set.");
            return Task.CompletedTask;
        }
        using var files = new TemporaryDirectory();
        var commands = new Commands(path, new EnvironmentAnimations(files.Combine("ENVIRONMENT_ANIMATION.DAT"), null!),
            new CollisionMaps(files.Combine("COLLISION.MAP"), null!, null!), new RenderableElements(files.Combine("REDS.BIN"), null!, null!));
        var player = commands.GetComposite(@"Required_Assets\Player_Torch_Light")!;
        AssertEx.True(player.functions.Any(f => f.function.AsFunctionType == FunctionType.TorchDynamicMovement));
        AssertEx.True(player.functions.Any(f => f.function.AsFunctionType == FunctionType.VariableThePlayer));
        var playerCount = player.GetEntities().Count();
        RipleyCharacterInstaller.EnsureRemoteTorch(commands);
        AssertRemote(commands.GetComposite(RipleyCharacterInstaller.RemoteTorchCompositeId)!);
        AssertOptics((FunctionEntity)player.GetEntityByID(new ShortGuid("52-BD-FB-F6")),
            commands.GetComposite(RipleyCharacterInstaller.RemoteTorchCompositeId)!.functions[0]);
        AssertEx.Equal(playerCount, player.GetEntities().Count());
        AssertEx.False(RipleyCharacterInstaller.EnsureRemoteTorch(commands));
        AssertRetailSerialization(path, files);
        return Task.CompletedTask;
    }

    private static void AssertRetailSerialization(string path, TemporaryDirectory files)
    {
        // Use the installer's real resource tables and Commands files. A new
        // synthetic Commands object has no retail format header or resources.
        var game = Path.GetFullPath(path);
        while (!Directory.Exists(Path.Combine(game, "DATA", "ENV")))
            game = Path.GetDirectoryName(game) ?? throw new InvalidDataException("Game root not found.");
        var level = Path.GetDirectoryName(Path.GetDirectoryName(path))!;
        var global = new Global(Path.Combine(game, "DATA", "ENV", "GLOBAL"),
            new PAK2(Path.Combine(game, "DATA", "GLOBAL", "ANIMATION.PAK")));
        var type = typeof(RipleyCharacterInstaller).GetNestedType("Assets", BindingFlags.NonPublic)!;
        var assets = Activator.CreateInstance(type, BindingFlags.Instance | BindingFlags.NonPublic,
            null, new object[] { level, global }, null)!;
        object Field(string name) => type.GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(assets)!;
        var sources = new[] { (Commands)Field("Commands") };
        foreach (var source in sources)
        {
            RipleyCharacterInstaller.EnsureRemoteTorch(source);
            var remote = source.GetComposite(RipleyCharacterInstaller.RemoteTorchCompositeId)!;
            var original = source.GetComposite(@"Required_Assets\Player_Torch_Light")!.GetEntityByID(remote.functions[0].shortGUID) as FunctionEntity;
            AssertOptics(original!, remote.functions[0]);
            AssertEx.True(original!.resources[0].RenderableInstance.Count > 0);
            // An already installed graph reloads with its own list object, but
            // must resolve every item to the same level render resource entry.
            AssertEx.Equal(original.resources[0].RenderableInstance.Count, remote.functions[0].resources[0].RenderableInstance.Count);
            for (var i = 0; i < original.resources[0].RenderableInstance.Count; ++i)
                AssertEx.True(ReferenceEquals(original.resources[0].RenderableInstance[i], remote.functions[0].resources[0].RenderableInstance[i]));
            var stagedPath = files.Combine(Path.GetFileName(source.Filepath));
            AssertEx.True(source.Save(stagedPath));
            var restored = new Commands(stagedPath, (EnvironmentAnimations)Field("Animations"),
                (CollisionMaps)Field("CollisionMaps"), (RenderableElements)Field("Renderables"));
            AssertEx.True(restored.Loaded, "Staged retail Commands must reload.");
            AssertEx.False(RipleyCharacterInstaller.EnsureRemoteTorch(restored));
            AssertRemote(restored.GetComposite(RipleyCharacterInstaller.RemoteTorchCompositeId)!);
            AssertOptics(original, restored.GetComposite(RipleyCharacterInstaller.RemoteTorchCompositeId)!.functions[0]);
            AssertEx.True(ReferenceEquals(original.resources[0].RenderableInstance[0],
                restored.GetComposite(RipleyCharacterInstaller.RemoteTorchCompositeId)!.functions[0].resources[0].RenderableInstance[0]));
            Console.WriteLine($"Remote torch round-trip verified against retail {Path.GetFileName(source.Filepath)} with resolved render resources.");
        }
    }
}
