<#
.SYNOPSIS
    Remove the bundled SudoVDA virtual display driver.

.DESCRIPTION
    Removes the device and, unless something else still uses it, the driver
    package. The Sunshine installer runs this, elevated, when the virtual
    display driver is removed or Sunshine is uninstalled.

    The signing certificate is left in the trusted stores on purpose: other
    software may have been installed under the same certificate, and removing
    it would break their signature checks. Remove it by hand if you want it
    gone.

    A transcript goes to %TEMP%\Sunshine\logs\sudovda.
#>

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir 'sudovda-driver.ps1')

$log = Open-SudoVdaLog -Action 'uninstall'
try {
    $result = Uninstall-SudoVdaDriver -RootDir (Split-Path -Parent $scriptDir)
    Write-SudoVdaStep "Result: $result"
} catch {
    Write-Warning "Could not remove the virtual display driver: $($_.Exception.Message)"
} finally {
    Close-SudoVdaLog -Path $log
}
exit 0
