using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace AIMP.Launcher;

internal static class InjectorRunner
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    internal static async Task<InjectorResult> RunAsync(
        string injectorPath,
        AiProcessTarget target,
        string dllPath,
        CancellationToken cancellationToken)
    {
        var startInfo = CreateStartInfo(injectorPath, target, dllPath);
        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("The x86 injector could not start.");
        var outputTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var errorTask = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        var result = ParseResult(await outputTask.ConfigureAwait(false), process.ExitCode);
        _ = await errorTask.ConfigureAwait(false);
        return result;
    }

    private static ProcessStartInfo CreateStartInfo(
        string injectorPath,
        AiProcessTarget target,
        string dllPath)
    {
        var info = new ProcessStartInfo(injectorPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        AddArguments(info, target, dllPath);
        return info;
    }

    private static void AddArguments(
        ProcessStartInfo info,
        AiProcessTarget target,
        string dllPath)
    {
        AddArgument(info, "--pid", target.ProcessId.ToString(CultureInfo.InvariantCulture));
        AddArgument(info, "--dll", dllPath);
        AddArgument(info, "--expected-exe", target.ExecutablePath);
        AddArgument(info, "--expected-start-utc-ticks",
            target.StartTimeUtcTicks.ToString(CultureInfo.InvariantCulture));
        AddArgument(info, "--timeout-ms", "15000");
    }

    private static void AddArgument(ProcessStartInfo info, string name, string value)
    {
        info.ArgumentList.Add(name);
        info.ArgumentList.Add(value);
    }

    private static InjectorResult ParseResult(string output, int exitCode)
    {
        var line = output.Split('\n', StringSplitOptions.RemoveEmptyEntries).LastOrDefault();
        var result = string.IsNullOrWhiteSpace(line)
            ? null
            : JsonSerializer.Deserialize<InjectorResult>(line, JsonOptions);
        if (result is not null) return result;
        return new InjectorResult(false, $"EXIT_{exitCode}", string.Empty);
    }
}
