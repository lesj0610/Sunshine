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

    Exits 0 when the driver is gone or was never there, 3010 when a restart
    finishes the removal, and 1 when it could not be removed. The installer
    ignores the code, so it never fails the Sunshine uninstall.

    A transcript goes to %TEMP%\Sunshine\logs\sudovda.
#>

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir 'sudovda-driver.ps1')

exit (Invoke-SudoVdaUninstall -RootDir (Split-Path -Parent $scriptDir))
