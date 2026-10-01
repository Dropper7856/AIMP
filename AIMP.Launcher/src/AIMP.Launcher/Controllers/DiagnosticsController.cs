using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/diagnostics")]
public sealed class DiagnosticsController : ControllerBase
{
    private readonly LauncherRuntimeService _runtime;

    public DiagnosticsController(LauncherRuntimeService runtime)
    {
        _runtime = runtime;
    }

    [HttpPost("export")]
    [ProducesResponseType<DiagnosticsExportResponse>(StatusCodes.Status200OK)]
    public Task<DiagnosticsExportResponse> Export(
        DiagnosticsExportRequest request,
        CancellationToken cancellationToken) =>
        _runtime.ExportDiagnosticsAsync(request.OutputPath, cancellationToken);
}
