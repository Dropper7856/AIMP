using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/application")]
public sealed class ApplicationController : ControllerBase
{
    private readonly IHostApplicationLifetime _lifetime;

    public ApplicationController(IHostApplicationLifetime lifetime)
    {
        _lifetime = lifetime;
    }

    [HttpPost("shutdown")]
    [ProducesResponseType(StatusCodes.Status202Accepted)]
    public IActionResult Shutdown()
    {
        HttpContext.Response.OnCompleted(() =>
        {
            _lifetime.StopApplication();
            return Task.CompletedTask;
        });
        return Accepted();
    }
}
