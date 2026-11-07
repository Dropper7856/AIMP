[CmdletBinding()]
param(
    [string]$CMakeExe = '',
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$OutputDirectory = 'artifacts/alien-api-437',
    [string]$SecondPcApi = '\\DESKTOP-2JNROA0\Users\Public\Documents\win-unpacked\resources\api',
    [switch]$DeliveryOnly
)

# Build and deliver the API only. No unit tests or Electron rebuild.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$api = Join-Path $output 'api'
$native = Join-Path $output 'native-vs2026'
$localApi = Join-Path $root 'dist/win-unpacked/resources/api'
New-Item -ItemType Directory -Path $output -Force | Out-Null

function Run([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed ($LASTEXITCODE)." }
}

function Copy-Api([string]$Destination) {
    # /E preserves files that exist only at the destination. Never use /MIR.
    & robocopy $api $Destination /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS | Out-Host
    if ($LASTEXITCODE -ge 8) { throw "API delivery failed: $Destination (robocopy $LASTEXITCODE)." }
    foreach ($name in @('AIMP.Launcher.dll', 'AIMP.Protocol.dll', 'AIMP.NetZ.dll', 'UIBridge/NEWFRONTENDMENU.GFX','UIBridge/PICKUPOVERLAY.GFX')) {
        $expected = (Get-FileHash -LiteralPath (Join-Path $api $name) -Algorithm SHA256).Hash
        $actual = (Get-FileHash -LiteralPath (Join-Path $Destination $name) -Algorithm SHA256).Hash
        if ($expected -ne $actual) { throw "SHA-256 mismatch: $Destination/$name" }
        [pscustomobject]@{ File=$name; Destination=$Destination; SHA256=$actual }
    }
}

Push-Location $root
try {
    if (!$DeliveryOnly) {
      if (!$CMakeExe) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($installation) {
                $candidate = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
                if (Test-Path -LiteralPath $candidate) { $CMakeExe = $candidate }
            }
        }
        if (!$CMakeExe) { $CMakeExe = (Get-Command cmake -ErrorAction Stop).Source }
      }
      Run $CMakeExe @('-S','AIMP.Native','-B',$native,'-G',$Generator,'-A','Win32','-DBUILD_TESTING=OFF')
      Run $CMakeExe @('--build',$native,'--config','Release','--target','AIMP.NetZ','--parallel','4')
      # Keep restore/build intermediates separate from IDE and other RID builds.
      Run 'dotnet' @('publish','AIMP.Launcher/src/AIMP.Launcher/AIMP.Launcher.csproj','-c','Release','-r','win-x64','--self-contained','true','--artifacts-path',(Join-Path $output 'dotnet'),'-o',$api)
      Run 'dotnet' @('publish','AIMP.Launcher/src/AIMP.Injector32/AIMP.Injector32.csproj','-c','Release','-r','win-x86','--self-contained','true','--artifacts-path',(Join-Path $output 'dotnet'),'-o',(Join-Path $api 'Injector32'))
      Copy-Item -LiteralPath (Join-Path $native 'bin/Release/AIMP.NetZ.dll') -Destination (Join-Path $api 'AIMP.NetZ.dll') -Force
    }
    foreach ($required in @('AIMP.Launcher.dll','AIMP.Protocol.dll','AIMP.NetZ.dll','UIBridge/NEWFRONTENDMENU.GFX','UIBridge/PICKUPOVERLAY.GFX','Injector32/AIMP.Injector32.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $api $required) -PathType Leaf)) { throw "Incomplete build artifacts: $required" }
    }

    $delivery = [ordered]@{ builtAtUtc=[DateTime]::UtcNow.ToString('o'); apiVersion='0.8.4.437'; protocol=37; unitTestsRun=$false; localDelivered=$false; secondPcDelivered=$false; hashes=@(); failures=@() }
    foreach ($target in @($localApi,$SecondPcApi)) {
        try {
            $delivery.hashes += @(Copy-Api $target)
            if ($target -eq $localApi) { $delivery.localDelivered=$true } else { $delivery.secondPcDelivered=$true }
        }
        catch { $delivery.failures += $_.Exception.Message; Write-Warning $_.Exception.Message }
    }
    $delivery | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'delivery.json') -Encoding UTF8
    if (!$delivery.localDelivered -or !$delivery.secondPcDelivered) {
        throw "Delivery incomplete. See $(Join-Path $output 'delivery.json')."
    }
    Write-Host 'API delivered to both PCs; SHA-256 equality verified.'
    # robocopy returns 1 when it copied files successfully. Do not leak that
    # success code as a build failure to callers inspecting LASTEXITCODE.
    $global:LASTEXITCODE = 0
}
finally { Pop-Location }
