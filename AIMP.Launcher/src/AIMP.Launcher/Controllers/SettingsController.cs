using AIMP.Launcher.Contracts;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/settings")]
public sealed class SettingsController : ControllerBase
{
    private readonly LauncherRuntimeService _runtime;

    public SettingsController(LauncherRuntimeService runtime)
    {
        _runtime = runtime;
    }

    [HttpGet]
    [ProducesResponseType<SettingsResponse>(StatusCodes.Status200OK)]
    public ActionResult<SettingsResponse> Get() => Ok(_runtime.GetSettings());

    [HttpPost("validate")]
    [ProducesResponseType<SettingsValidationResponse>(StatusCodes.Status200OK)]
    public ActionResult<SettingsValidationResponse> Validate(UpdateSettingsRequest request) =>
        Ok(_runtime.ValidateSettings(request));

    [HttpPut]
    [ProducesResponseType<SettingsResponse>(StatusCodes.Status200OK)]
    public ActionResult<SettingsResponse> Update(UpdateSettingsRequest request) =>
        Ok(_runtime.SaveSettings(request));
}
