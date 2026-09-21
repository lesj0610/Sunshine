<#
.SYNOPSIS
    Remove the bundled SudoVDA virtual display driver.

.DESCRIPTION
    Runs as part of the Sunshine uninstaller, which already invokes it elevated.
    Removes the device node.

    The signing certificate is left in the trusted stores on purpose: other
    software may have been installed under the same certificate, and removing it
    would break their signature checks. Remove it by hand if you want it gone.
#>

[CmdletBinding()]
param(
    [switch] $Silent
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir    = Split-Path -Parent $scriptDir
$driverDir  = Join-Path $rootDir 'drivers\sudovda'
$hardwareId = 'root\sudomaker\sudovda'
$classGuid  = '{4D36E968-E325-11CE-BFC1-08002BE10318}'

function Get-VirtualDisplayDevice {
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like 'ROOT\SUDOMAKER\SUDOVDA*' }
}

try {
    if (-not (Get-VirtualDisplayDevice)) {
        exit 0
    }

    $nefcon = Join-Path $driverDir 'nefconc.exe'
    if (-not (Test-Path -LiteralPath $nefcon)) {
        Write-Warning "nefconc.exe not found in $driverDir, leaving the device in place."
        exit 0
    }

    & $nefcon --remove-device-node --hardware-id $hardwareId --class-guid $classGuid | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "nefconc exit code $LASTEXITCODE"
    }

    $remaining = $null
    foreach ($attempt in 1..20) {
        Start-Sleep -Milliseconds 500
        $remaining = Get-VirtualDisplayDevice
        if (-not $remaining) { break }
    }
    if ($remaining) {
        throw "the device is still present: $($remaining.InstanceId)"
    }

    Write-Information '  Virtual display driver removed' -InformationAction Continue
    exit 0

} catch {
    Write-Warning "Could not remove the virtual display driver: $($_.Exception.Message)"
    exit 0
}
