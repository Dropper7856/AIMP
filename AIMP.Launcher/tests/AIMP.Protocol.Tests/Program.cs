namespace AIMP.Protocol.Tests;

internal static class Program
{
    private static readonly TestCase[] Tests =
    {
        new("Protocol frames", ProtocolTests.FrameRoundTripAndValidationAsync),
        new("Protocol models", ProtocolTests.ModelValidationAsync),
        new("AISync authority codec", AISyncCodecTests.AuthorityStateRoundTripAsync),
        new("WS3 wire, authority and manifest", ProtocolTests.WorldPublicActionContractAsync),
        new("WS3 local effect boundary", WorldPublicActionTests.LocalEffectBoundaryAsync),
        new("Lobby lifecycle", ProtocolTests.LobbyLifecycleAsync),
        new("Lobby manifest preflight", PreparationTests.LobbyManifestPreflightAsync),
        new("Session loading barrier", PreparationTests.SessionReleaseBarrierAsync),
        new("AI process selection", LauncherTests.ProcessSelectionAsync),
        new("Native DLL priority", LauncherTests.NativeDllPriorityAsync),
        new("Native module matching", LauncherTests.NativeModuleMatchingAsync),
        new("Diagnostics export", LauncherTests.DiagnosticsExportAsync),
        new("Command result diagnostics", LauncherTests.CommandResultDiagnosticsAsync),
        new("Character movement tools", LauncherTests.CharacterMovementToolsAsync),
        new("Local player position tools", LauncherTests.LocalPlayerPositionToolsAsync),
        new("Launcher integration tools", LauncherTests.LauncherIntegrationToolsAsync),
        new("Generic native IPC", IpcTests.GenericHandshakeAndQueuedCommandAsync),
        new("AISync rejection preserves IPC", IpcTests.AISyncRejectionPreservesConnectionAsync),
        new("Launcher body length", ArchitectureTests.LauncherBodiesAreShortAsync)
    };

    [STAThread]
    private static async Task<int> Main(string[] args)
    {
        var tests = SelectedTests();
        if (args.Length != 0)
        {
            if (args.Length != 2 || args[0] != "--filter")
                throw new ArgumentException("Usage: --filter <test name>");
            var filters = args[1].Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
            tests = tests.Where(test => filters.Any(filter => test.Name.Contains(filter, StringComparison.OrdinalIgnoreCase))).ToArray();
            if (tests.Length == 0) throw new ArgumentException("No tests matched the filter.");
        }
        var failures = new List<string>();
        foreach (var test in tests)
        {
            await RunAsync(test, failures).ConfigureAwait(false);
        }

        Console.WriteLine($"{tests.Length - failures.Count}/{tests.Length} tests passed.");
        return failures.Count == 0 ? 0 : 1;
    }

    private static TestCase[] SelectedTests()
    {
        if (!string.Equals(Environment.GetEnvironmentVariable(
                "AIMP_REQUIRE_DEPLOYMENT_TEST"), "1", StringComparison.Ordinal))
        {
            return Tests;
        }

        var tests = Tests.ToList();
        tests.Insert(5, new TestCase(
            "Native DLL deployment", LauncherTests.NativeDllDeploymentAsync));
        return tests.ToArray();
    }

    private static async Task RunAsync(TestCase test, ICollection<string> failures)
    {
        try
        {
            await test.Run().ConfigureAwait(false);
            Console.WriteLine($"PASS {test.Name}");
        }
        catch (Exception exception)
        {
            failures.Add(test.Name);
            Console.Error.WriteLine($"FAIL {test.Name}: {exception}");
        }
    }
}

internal sealed record TestCase(string Name, Func<Task> Run);

internal static class AssertEx
{
    internal static void True(bool condition, string? message = null)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message ?? "Expected condition to be true.");
        }
    }

    internal static void False(bool condition, string? message = null) =>
        True(!condition, message ?? "Expected condition to be false.");

    internal static void Equal<T>(T expected, T actual, string? message = null)
        where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException(
                message ?? $"Expected '{expected}', received '{actual}'.");
        }
    }

    internal static void SequenceEqual<T>(
        IEnumerable<T> expected,
        IEnumerable<T> actual,
        string? message = null)
    {
        if (!expected.SequenceEqual(actual))
        {
            throw new InvalidOperationException(message ?? "Sequences differ.");
        }
    }

    internal static TException Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException exception)
        {
            return exception;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    internal static async Task<TException> ThrowsAsync<TException>(Func<Task> action)
        where TException : Exception
    {
        try
        {
            await action().ConfigureAwait(false);
        }
        catch (TException exception)
        {
            return exception;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }
}

internal sealed class TemporaryDirectory : IDisposable
{
    internal TemporaryDirectory()
    {
        DirectoryPath = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(),
            "AIMP.Protocol.Tests",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(DirectoryPath);
    }

    internal string DirectoryPath { get; }

    internal string Combine(params string[] parts)
    {
        var values = new[] { DirectoryPath }.Concat(parts).ToArray();
        return System.IO.Path.Combine(values);
    }

    public void Dispose()
    {
        if (Directory.Exists(DirectoryPath))
        {
            Directory.Delete(DirectoryPath, true);
        }
    }
}
