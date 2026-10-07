<#
.SYNOPSIS
    Install the bundled SudoVDA virtual display driver.

.DESCRIPTION
    Verifies the driver files, trusts the signing certificate, creates the
    device node and installs the driver, then confirms the device came up. A
    failure undoes the steps already taken, and never fails the Sunshine
    install: Sunshine works without a virtual display.

    The Sunshine installer runs this, elevated and with -Silent, only when the
    virtual display driver was chosen in it. Run by hand, from an elevated
    console, it asks first. With nobody to ask and no -Silent it changes
    nothing.

    Exits 0 when there is nothing wrong (including a decline, or a machine the
    driver does not support), 3010 when a restart finishes the install, and 1
    when it did not happen. The installer ignores the code, so it never fails
    the Sunshine install; it is there for whoever runs this by hand.

    A transcript goes to %TEMP%\Sunshine\logs\sudovda.
#>

[CmdletBinding()]
param(
    # Install without asking.
    [switch] $Silent
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptDir 'sudovda-driver.ps1')

exit (Invoke-SudoVdaInstall -RootDir (Split-Path -Parent $scriptDir) -Silent:$Silent)
