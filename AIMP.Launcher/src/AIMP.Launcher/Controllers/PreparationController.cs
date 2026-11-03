using AIMP.Launcher.Contracts;
using Microsoft.AspNetCore.Mvc;

namespace AIMP.Launcher.Controllers;

[ApiController]
[Route("api/v1/preparation")]
public sealed class PreparationController : ControllerBase
{
    private readonly GamePreparationService _preparation;
    public PreparationController(GamePreparationService preparation) => _preparation = preparation;

    [HttpGet]
    public ActionResult<GamePreparationDto> Get() => Ok(_preparation.GetStatus());

    [HttpPost]
    public ActionResult<GamePreparationDto> Start() => Ok(_preparation.Start(revalidate: true));

    [HttpPost("skip")]
    public ActionResult<GamePreparationDto> Skip() => Ok(_preparation.Skip());
}
