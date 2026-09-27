using System.ComponentModel;
using System.Globalization;
using System.Text.Json;

namespace AIMP.Injector32;

internal static class Program
{
    private const int Success = 0;
    private const int InvalidArguments = 2;
    private const int ValidationFailure = 3;
    private const int InjectionFailure = 4;
    private const int DefaultTimeoutMilliseconds = 15_000;
    private const string Usage = "Usage: AIMP.Injector32 --pid <pid> --dll <path> "
        + "--expected-exe <AI.exe> --expected-start-utc-ticks <ticks> "
        + "[--timeout-ms <ms>].";

    public static int Main(string[] args)
    {
        try
        {
            return Run(args);
        }
        catch (Exception exception)
        {
            return ReportFailure(exception);
        }
    }

    private static int Run(string[] args)
    {
        EnsureX86Process();
        var options = Options.Parse(args);
        var result = Inject(options);
        WriteResult(true, "OK", result);
        return Success;
    }

    private static string Inject(Options options) => NativeInjector.Inject(
        options.ProcessId,
        options.DllPath,
        options.ExpectedExecutablePath,
        options.ExpectedProcessStartUtcTicks,
        TimeSpan.FromMilliseconds(options.TimeoutMilliseconds));

    private static void EnsureX86Process()
    {
        if (Environment.Is64BitProcess)
        {
            throw new InvalidOperationException(
                "AIMP.Injector32 must run as an x86 process.");
        }
    }

    private static int ReportFailure(Exception exception)
    {
        var (code, exitCode) = ClassifyFailure(exception);
        WriteResult(false, code, exception.Message);
        return exitCode;
    }

    private static (string Code, int ExitCode) ClassifyFailure(Exception exception) =>
        exception switch
        {
            ArgumentException or FormatException or OverflowException =>
                ("INVALID_ARGUMENT", InvalidArguments),
            Win32Exception => ("INJECTION_FAILED", InjectionFailure),
            InvalidOperationException or FileNotFoundException or BadImageFormatException =>
                ("VALIDATION_FAILED", ValidationFailure),
            _ => ("INJECTION_FAILED", InjectionFailure)
        };

    private static void WriteResult(bool success, string code, string message) =>
        Console.WriteLine(JsonSerializer.Serialize(new { success, code, message }));

    private sealed record Options(
        int ProcessId,
        string DllPath,
        string ExpectedExecutablePath,
        long ExpectedProcessStartUtcTicks,
        int TimeoutMilliseconds)
    {
        private static readonly HashSet<string> SupportedArguments =
        [
            "--pid",
            "--dll",
            "--expected-exe",
            "--expected-start-utc-ticks",
            "--timeout-ms"
        ];

        public static Options Parse(string[] args)
        {
            ArgumentNullException.ThrowIfNull(args);
            var values = ParsePairs(args);
            var options = Create(values);
            options.Validate();
            return options;
        }

        private static Options Create(IReadOnlyDictionary<string, string> values) =>
            new(
                ReadInt(values, "--pid"),
                ReadPath(values, "--dll"),
                ReadPath(values, "--expected-exe"),
                ReadLong(values, "--expected-start-utc-ticks"),
                ReadInt(values, "--timeout-ms", DefaultTimeoutMilliseconds));

        private void Validate()
        {
            if (ProcessId <= 0 || ExpectedProcessStartUtcTicks <= 0
                || TimeoutMilliseconds is < 1_000 or > 60_000)
            {
                throw new ArgumentException(
                    "PID, DLL path, expected AI.exe path, process creation identity, "
                    + "and a timeout from 1000 to 60000 ms are required.");
            }
        }

        private static Dictionary<string, string> ParsePairs(string[] args)
        {
            if (args.Length == 0 || args.Length % 2 != 0)
            {
                throw new ArgumentException(Usage);
            }

            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            for (var index = 0; index < args.Length; index += 2)
            {
                AddPair(values, args[index], args[index + 1]);
            }

            return values;
        }

        private static void AddPair(
            IDictionary<string, string> values,
            string name,
            string value)
        {
            if (!SupportedArguments.Contains(name) || string.IsNullOrWhiteSpace(value)
                || !values.TryAdd(name, value))
            {
                throw new ArgumentException(Usage);
            }
        }

        private static string ReadRequired(
            IReadOnlyDictionary<string, string> values,
            string name)
        {
            if (values.TryGetValue(name, out var value))
            {
                return value;
            }

            throw new ArgumentException($"Missing required argument: {name}. {Usage}");
        }

        private static int ReadInt(
            IReadOnlyDictionary<string, string> values,
            string name) => int.Parse(
                ReadRequired(values, name),
                NumberStyles.None,
                CultureInfo.InvariantCulture);

        private static int ReadInt(
            IReadOnlyDictionary<string, string> values,
            string name,
            int defaultValue) => values.TryGetValue(name, out var value)
                ? int.Parse(value, NumberStyles.None, CultureInfo.InvariantCulture)
                : defaultValue;

        private static long ReadLong(
            IReadOnlyDictionary<string, string> values,
            string name) => long.Parse(
                ReadRequired(values, name),
                NumberStyles.None,
                CultureInfo.InvariantCulture);

        private static string ReadPath(
            IReadOnlyDictionary<string, string> values,
            string name) => Path.GetFullPath(ReadRequired(values, name));
    }
}
