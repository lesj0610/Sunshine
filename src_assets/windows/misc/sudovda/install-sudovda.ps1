<#
.SYNOPSIS
    Install the bundled SudoVDA virtual display driver.

.DESCRIPTION
    Verifies the driver files, trusts the signing certificate, creates the
    device node and installs the driver, then confirms the device came up. A
    failure undoes the steps already taken.

    The Sunshine installer runs this, elevated and with -Silent, only when the
    virtual display driver was chosen in it. Run by hand, from an elevated
    console, it asks first. With nobody to ask and no -Silent it changes
    nothing.

    Exits 0 when there is nothing wrong (including a decline, or a machine the
    driver does not support), 3010 when a restart finishes the install, and 1
    when it did not happen. The installer passes -Installer as well, and takes
    any code but 0 as a failure: it fails the Sunshine install, which is rolled
    back.

    A transcript goes to %TEMP%\Sunshine\logs\sudovda.
#>

[CmdletBinding()]
param(
    # Install without asking.
    [switch] $Silent,

    # Run by the Sunshine installer, which fails on any exit code but 0. A
    # restart that finishes the install is not a failure, so it exits 0 then.
    [switch] $Installer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir 'sudovda-driver.ps1')

$exitCode = Invoke-SudoVdaInstall -RootDir (Split-Path -Parent $scriptDir) -Silent:$Silent
if ($Installer -and $exitCode -eq $SudoVdaRebootRequired) {
    $exitCode = 0
}
exit $exitCode
