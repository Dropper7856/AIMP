namespace AIMP.Launcher;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        if (WorldManifestCli.TryRun(args, out var toolExitCode))
        {
            Environment.ExitCode = toolExitCode;
            return;
        }

        try
        {
            RunLauncher();
        }
        catch (Exception exception)
        {
            ReportStartupFailure(exception);
        }
    }

    private static void RunLauncher()
    {
        ApplicationConfiguration.Initialize();
        using var diagnostics = new LauncherDiagnostics();
        using var controller = new NativeRuntimeController(diagnostics);
        using var form = new MainForm(controller);
        Application.Run(form);
        controller.ShutdownAsync().GetAwaiter().GetResult();
    }

    private static void ReportStartupFailure(Exception exception)
    {
        Environment.ExitCode = 1;
        var message = $"The launcher could not start.{Environment.NewLine}{exception.Message}";
        MessageBox.Show(message, "AIMP Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
    }
}
