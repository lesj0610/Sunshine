<#
.SYNOPSIS
    Install the bundled SudoVDA virtual display driver.

.DESCRIPTION
    Runs as part of the Sunshine installer, which already invokes it elevated.
    Verifies the driver files, trusts the signing certificate, creates the
    device node and installs the driver, then confirms the device came up.

    Every step is checked, and a failure undoes the steps already taken rather
    than leaving the machine half configured. A failure here never fails the
    Sunshine install: Sunshine works without a virtual display, so this reports
    and returns instead of aborting.
#>

[CmdletBinding()]
param(
    # Do not prompt. The Sunshine installer passes this for unattended installs.
    [switch] $Silent
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir    = Split-Path -Parent $scriptDir
$driverDir  = Join-Path $rootDir 'drivers\sudovda'
$hardwareId = 'root\sudomaker\sudovda'
$classGuid  = '{4D36E968-E325-11CE-BFC1-08002BE10318}'

# SHA-256 of the driver files as shipped. Re-checked on every install so a
# tampered or truncated payload never reaches the certificate store.
$expectedHashes = @{
    'SudoVDA.dll' = '47EE263CB5DE9382C6630A2D7F3DAFEC4A49419F953BEEC869CA5DD0C460FF63'
    'SudoVDA.inf' = 'AD69AC682756F0CF339B081FAC7E6E8159FDF2CA01CA69DF8945C7246C286925'
    'sudovda.cat' = '2F9189DE5604BEC9D86F51640CC540639E394D9AD0F8E689129375E95F2D22F8'
    'sudovda.cer' = '6ACCDCD519F6179D967DB4EAA20ECF25A732BA30E87F4CFFEBC768B2C13C9007'
    'nefconc.exe' = '9DBA1F1A9E2B21843C4A0CA6C6FFA9E747250DD6E42C8C14325CA069AD43EA8F'
}

$certThumbprint = '6ACCDCD519F6179D967DB4EAA20ECF25A732BA30E87F4CFFEBC768B2C13C9007'

$undo = [System.Collections.ArrayList]::new()

function Write-Step {
    param([string] $Message)
    Write-Information "  $Message" -InformationAction Continue
}

function Invoke-Rollback {
    if ($undo.Count -eq 0) { return }
    Write-Step 'Rolling back the virtual display driver install'
    for ($i = $undo.Count - 1; $i -ge 0; $i--) {
        try {
            & $undo[$i].Action
            Write-Step "  undone: $($undo[$i].Description)"
        } catch {
            Write-Warning "  could not undo $($undo[$i].Description): $($_.Exception.Message)"
        }
    }
}

function Get-VirtualDisplayDevice {
    # nefconc creates the device node from the class name, so Windows names
    # the instance ROOT\DISPLAY\nnnn. Only the hardware ID says it is SudoVDA.
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -contains 'root\sudomaker\sudovda' }
}

try {
    if (-not (Test-Path -LiteralPath $driverDir)) {
        Write-Warning "SudoVDA driver files not found in $driverDir, skipping."
        exit 0
    }

    if (Get-VirtualDisplayDevice) {
        Write-Step 'SudoVDA is already installed, leaving it as it is.'
        exit 0
    }

    foreach ($name in $expectedHashes.Keys) {
        $path = Join-Path $driverDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            throw "missing driver file: $name"
        }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($actual -ne $expectedHashes[$name]) {
            throw "$name does not match its expected SHA-256"
        }
    }
    Write-Step 'Driver files verified'

    if (-not $Silent) {
        Write-Information '' -InformationAction Continue
        Write-Information '  The virtual display driver is signed with a self-signed certificate:' -InformationAction Continue
        Write-Information '    subject     CN=sudovda@su.mk' -InformationAction Continue
        Write-Information "    thumbprint  $certThumbprint" -InformationAction Continue
        Write-Information '    stores      Root and TrustedPublisher' -InformationAction Continue
        Write-Information '' -InformationAction Continue
        Write-Information '  Windows will then trust anything signed by that certificate, not only' -InformationAction Continue
        Write-Information '  this driver. The driver itself is user-mode (UMDF/IddCx).' -InformationAction Continue
        Write-Information '' -InformationAction Continue
        $answer = Read-Host '  Install the virtual display driver? [Y/n]'
        if ($answer -match '^(n|no)$') {
            Write-Step 'Skipped at your request. Sunshine works without it.'
            exit 0
        }
    }

    # --- certificate ---
    $certPath = Join-Path $driverDir 'sudovda.cer'
    foreach ($store in @('Root', 'TrustedPublisher')) {
        $present = Get-ChildItem "Cert:\LocalMachine\$store" |
                   Where-Object { $_.Thumbprint -eq $certThumbprint }
        if ($present) { continue }

        $null = Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\LocalMachine\$store"
        $storeName = $store
        [void] $undo.Add([pscustomobject]@{
            Description = "certificate from $storeName"
            Action      = {
                Get-ChildItem "Cert:\LocalMachine\$storeName" |
                    Where-Object { $_.Thumbprint -eq $certThumbprint } |
                    Remove-Item -Force
            }.GetNewClosure()
        })
    }
    Write-Step 'Signing certificate trusted'

    # --- driver ---
    $nefcon = Join-Path $driverDir 'nefconc.exe'
    $inf    = Join-Path $driverDir 'SudoVDA.inf'

    & $nefcon --create-device-node --class-name Display --class-guid $classGuid --hardware-id $hardwareId | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "could not create the device node (nefconc exit code $LASTEXITCODE)"
    }
    [void] $undo.Add([pscustomobject]@{
        Description = 'device node'
        Action      = {
            & $nefcon --remove-device-node --hardware-id $hardwareId --class-guid $classGuid | Out-Null
        }.GetNewClosure()
    })

    & $nefcon --install-driver --inf-path $inf | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "could not install the driver (nefconc exit code $LASTEXITCODE)"
    }

    # --- verify ---
    $device = $null
    foreach ($attempt in 1..20) {
        Start-Sleep -Milliseconds 500
        $device = Get-VirtualDisplayDevice
        if ($device -and $device.Status -eq 'OK') { break }
    }
    if (-not $device) {
        throw 'the driver installed but no device appeared'
    }
    if ($device.Status -ne 'OK') {
        throw "the device is present but its status is '$($device.Status)'"
    }

    Write-Step "Virtual display driver installed: $($device.InstanceId)"
    exit 0

} catch {
    Write-Warning "Virtual display driver install failed: $($_.Exception.Message)"
    Invoke-Rollback
    Write-Warning 'Sunshine itself is unaffected and will continue without a virtual display.'
    exit 0
}
