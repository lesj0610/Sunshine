<#
.SYNOPSIS
    Install and remove the bundled SudoVDA virtual display driver.

.DESCRIPTION
    Functions only: install-sudovda.ps1 and uninstall-sudovda.ps1 dot-source
    this and call into it, and the tests dot-source it to drive it without a
    driver. Nothing runs when it is loaded.

    Every install step is checked, and a failure undoes the steps already taken
    rather than leaving the machine half configured. Nothing here fails the
    Sunshine install: Sunshine works without a virtual display, so failures are
    reported and returned instead of thrown.
#>

# How Windows knows the device.
$SudoVdaHardwareId      = 'root\sudomaker\sudovda'
$SudoVdaInstancePattern = 'ROOT\SUDOMAKER\SUDOVDA*'
$SudoVdaDisplayClass    = '{4D36E968-E325-11CE-BFC1-08002BE10318}'

# The driver is built for AMD64 only. Its INF has no other platform section,
# so on anything else the install can only fail, after the certificate has
# already been trusted.
$SudoVdaArchitecture = 'AMD64'

# SHA-256 of each shipped file. Checked before anything is trusted, so a
# tampered or truncated payload never reaches the certificate store.
$SudoVdaFileHashes = [ordered]@{
    'SudoVDA.dll' = '47EE263CB5DE9382C6630A2D7F3DAFEC4A49419F953BEEC869CA5DD0C460FF63'
    'SudoVDA.inf' = 'AD69AC682756F0CF339B081FAC7E6E8159FDF2CA01CA69DF8945C7246C286925'
    'sudovda.cat' = '2F9189DE5604BEC9D86F51640CC540639E394D9AD0F8E689129375E95F2D22F8'
    'sudovda.cer' = '6ACCDCD519F6179D967DB4EAA20ECF25A732BA30E87F4CFFEBC768B2C13C9007'
    'nefconc.exe' = '9DBA1F1A9E2B21843C4A0CA6C6FFA9E747250DD6E42C8C14325CA069AD43EA8F'
}

# The certificate the driver catalog is signed with, as the certificate stores
# identify it: by SHA-1 thumbprint. The SHA-256 above is of the .cer file and
# only serves the integrity check; the stores never report it.
$SudoVdaCertificateThumbprint = '3C918FC73525AD8B1521B6DB26B71F694277CC49'
$SudoVdaCertificateStores     = @('Root', 'TrustedPublisher')

function Write-SudoVdaStep {
    param([string] $Message)
    Write-Information "  $Message" -InformationAction Continue
}

function Get-ProcessorArchitectureCode {
    <#
    .SYNOPSIS
    Win32_Processor's Architecture code, or nothing if there is no processor.
    #>
    $processor = Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop | Select-Object -First 1
    if ($processor) {
        return [int] $processor.Architecture
    }
    return $null
}

function Get-NativeArchitecture {
    <#
    .SYNOPSIS
    The processor's architecture, not the process's: an emulated PowerShell
    reports the architecture it emulates.
    #>
    try {
        $code = Get-ProcessorArchitectureCode
        if ($null -ne $code) {
            switch ($code) {
                0  { return 'x86' }
                5  { return 'ARM' }
                9  { return 'AMD64' }
                12 { return 'ARM64' }
            }
        }
    } catch {
        Write-Verbose "Win32_Processor unavailable: $($_.Exception.Message)"
    }

    if ($env:PROCESSOR_ARCHITEW6432) {
        return $env:PROCESSOR_ARCHITEW6432
    }
    return $env:PROCESSOR_ARCHITECTURE
}

function Get-SudoVdaDevice {
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like $SudoVdaInstancePattern }
}

function Wait-SudoVdaDevice {
    <#
    .SYNOPSIS
    Wait for the device to come up after the driver install.

    .OUTPUTS
    The device as last seen, or nothing if it never appeared.
    #>
    $device = $null
    foreach ($attempt in 1..20) {
        Start-Sleep -Milliseconds 500
        $device = Get-SudoVdaDevice | Select-Object -First 1
        if ($device -and $device.Status -eq 'OK') {
            break
        }
    }
    return $device
}

function Get-SudoVdaCertificate {
    param([Parameter(Mandatory)] [string] $Store)
    Get-ChildItem -Path "Cert:\LocalMachine\$Store" |
        Where-Object { $_.Thumbprint -eq $SudoVdaCertificateThumbprint }
}

function Add-SudoVdaCertificate {
    param(
        [Parameter(Mandatory)] [string] $Store,
        [Parameter(Mandatory)] [string] $CertificatePath
    )
    $null = Import-Certificate -FilePath $CertificatePath -CertStoreLocation "Cert:\LocalMachine\$Store"
}

function Remove-SudoVdaCertificate {
    [CmdletBinding(SupportsShouldProcess)]
    param([Parameter(Mandatory)] [string] $Store)
    if ($PSCmdlet.ShouldProcess("Cert:\LocalMachine\$Store\$SudoVdaCertificateThumbprint", 'Remove certificate')) {
        Get-SudoVdaCertificate -Store $Store | Remove-Item -Force
    }
}

function Test-SudoVdaPayload {
    <#
    .SYNOPSIS
    Throw unless every driver file is present and matches its SHA-256.
    #>
    param([Parameter(Mandatory)] [string] $DriverDir)

    foreach ($name in $SudoVdaFileHashes.Keys) {
        $path = Join-Path $DriverDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            throw "missing driver file: $name"
        }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($actual -ne $SudoVdaFileHashes[$name]) {
            throw "$name does not match its expected SHA-256"
        }
    }
}

function Test-CanPrompt {
    <#
    .SYNOPSIS
    Whether someone can see a question and answer it.

    .DESCRIPTION
    Under the MSI the output is captured and nobody is at the console, so a
    question would either hang unseen or fail. Only a console that is really
    there counts.
    #>
    try {
        return [Environment]::UserInteractive -and
            -not [Console]::IsInputRedirected -and
            -not [Console]::IsOutputRedirected
    } catch {
        return $false
    }
}

function Invoke-Nefcon {
    <#
    .SYNOPSIS
    Run nefconc, log what it says, and return its exit code.
    #>
    param(
        [Parameter(Mandatory)] [string] $NefconPath,
        [Parameter(Mandatory)] [string[]] $Arguments
    )

    # nefconc may write to stderr. That is output to log, not an error to stop
    # on, which is what Windows PowerShell makes of it under 'Stop'.
    $ErrorActionPreference = 'Continue'
    $output = & $NefconPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    foreach ($line in $output) {
        $text = "$line".Trim()
        if ($text) {
            Write-SudoVdaStep "  nefconc: $text"
        }
    }
    return $exitCode
}

function Write-SudoVdaConsentText {
    Write-SudoVdaStep ''
    Write-SudoVdaStep 'The virtual display driver is signed with a self-signed certificate:'
    Write-SudoVdaStep '  subject     CN=sudovda@su.mk'
    Write-SudoVdaStep "  thumbprint  $SudoVdaCertificateThumbprint"
    Write-SudoVdaStep '  stores      LocalMachine Root and TrustedPublisher'
    Write-SudoVdaStep ''
    Write-SudoVdaStep 'Windows will then trust anything signed by that certificate, not only'
    Write-SudoVdaStep 'this driver, and uninstalling Sunshine keeps it. The driver itself is'
    Write-SudoVdaStep 'user-mode (UMDF/IddCx).'
    Write-SudoVdaStep ''
}

function Undo-SudoVdaInstall {
    <#
    .SYNOPSIS
    Undo the recorded install steps, newest first.
    #>
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [System.Collections.IList] $Steps,
        [Parameter(Mandatory)] [string] $NefconPath
    )

    if ($Steps.Count -eq 0) {
        return
    }

    Write-SudoVdaStep 'Rolling back the virtual display driver install'
    for ($i = $Steps.Count - 1; $i -ge 0; $i--) {
        $step = $Steps[$i]
        try {
            switch ($step.Kind) {
                'certificate' {
                    Remove-SudoVdaCertificate -Store $step.Store
                }
                'device-node' {
                    $code = Invoke-Nefcon -NefconPath $NefconPath -Arguments @(
                        '--remove-device-node',
                        '--hardware-id', $SudoVdaHardwareId,
                        '--class-guid', $SudoVdaDisplayClass
                    )
                    if ($code -ne 0) {
                        throw "nefconc exit code $code"
                    }
                }
            }
            Write-SudoVdaStep "  undone: $($step.Description)"
        } catch {
            Write-Warning "  could not undo $($step.Description): $($_.Exception.Message)"
        }
    }
}

function Install-SudoVdaDriver {
    <#
    .SYNOPSIS
    Install the driver found under a Sunshine install root.

    .PARAMETER RootDir
    The Sunshine install root, holding drivers\sudovda.

    .PARAMETER Silent
    Install without asking. The Sunshine installer passes this when the driver
    was chosen in it; that choice is the consent. Without it the question is
    asked, and only when someone can answer it.

    .OUTPUTS
    What happened: installed, already-installed, skipped-no-files,
    skipped-architecture, not-asked, declined or failed.
    #>
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [string] $RootDir,
        [switch] $Silent
    )

    $driverDir = Join-Path $RootDir 'drivers\sudovda'
    $nefcon    = Join-Path $driverDir 'nefconc.exe'
    $inf       = Join-Path $driverDir 'SudoVDA.inf'

    if (-not (Test-Path -LiteralPath $driverDir)) {
        Write-Warning "SudoVDA driver files not found in $driverDir, skipping."
        return 'skipped-no-files'
    }

    $architecture = Get-NativeArchitecture
    if ($architecture -ne $SudoVdaArchitecture) {
        Write-Warning (("The virtual display driver is built for {0} only and this machine is {1}. " +
                        "Skipping it; nothing was changed.") -f $SudoVdaArchitecture, $architecture)
        return 'skipped-architecture'
    }

    if (Get-SudoVdaDevice) {
        Write-SudoVdaStep 'SudoVDA is already installed, leaving it as it is.'
        return 'already-installed'
    }

    $undo = [System.Collections.ArrayList]::new()
    try {
        Test-SudoVdaPayload -DriverDir $driverDir
        Write-SudoVdaStep 'Driver files verified'

        if (-not $Silent) {
            if (-not (Test-CanPrompt)) {
                Write-Warning ('Not installing the virtual display driver: nobody can be asked, and trusting ' +
                               'its certificate needs consent. Run it again with -Silent to install it without asking.')
                return 'not-asked'
            }
            Write-SudoVdaConsentText
            $answer = Read-Host '  Install the virtual display driver? [Y/n]'
            if ($answer -match '^(n|no)$') {
                Write-SudoVdaStep 'Skipped at your request. Sunshine works without it.'
                return 'declined'
            }
        }

        # --- certificate ---
        # Only a certificate this install added is recorded, so a rollback never
        # takes away one that was trusted before.
        $certificatePath = Join-Path $driverDir 'sudovda.cer'
        foreach ($store in $SudoVdaCertificateStores) {
            if (Get-SudoVdaCertificate -Store $store) {
                continue
            }
            Add-SudoVdaCertificate -Store $store -CertificatePath $certificatePath
            [void] $undo.Add([pscustomobject]@{
                Kind        = 'certificate'
                Store       = $store
                Description = "certificate from $store"
            })
        }
        Write-SudoVdaStep 'Signing certificate trusted'

        # --- driver ---
        $code = Invoke-Nefcon -NefconPath $nefcon -Arguments @(
            '--create-device-node',
            '--class-name', 'Display',
            '--class-guid', $SudoVdaDisplayClass,
            '--hardware-id', $SudoVdaHardwareId
        )
        if ($code -ne 0) {
            throw "could not create the device node (nefconc exit code $code)"
        }
        [void] $undo.Add([pscustomobject]@{
            Kind        = 'device-node'
            Store       = $null
            Description = 'device node'
        })

        $code = Invoke-Nefcon -NefconPath $nefcon -Arguments @('--install-driver', '--inf-path', $inf)
        if ($code -ne 0) {
            throw "could not install the driver (nefconc exit code $code)"
        }

        # --- verify ---
        $device = Wait-SudoVdaDevice
        if (-not $device) {
            throw 'the driver installed but no device appeared'
        }
        if ($device.Status -ne 'OK') {
            throw "the device is present but its status is '$($device.Status)'"
        }

        Write-SudoVdaStep "Virtual display driver installed: $($device.InstanceId)"
        return 'installed'
    } catch {
        Write-Warning "Virtual display driver install failed: $($_.Exception.Message)"
        Undo-SudoVdaInstall -Steps $undo -NefconPath $nefcon
        Write-Warning 'Sunshine itself is unaffected and will continue without a virtual display.'
        return 'failed'
    }
}

function Uninstall-SudoVdaDriver {
    <#
    .SYNOPSIS
    Remove the driver installed from a Sunshine install root.

    .DESCRIPTION
    nefconc removes the device and then the driver package, unless something
    else still uses it. The signing certificate stays in the trusted stores on
    purpose: other software may have been installed under the same
    certificate, and removing it would break their signature checks.

    .OUTPUTS
    What happened: removed, not-installed, missing-tool or failed.
    #>
    [CmdletBinding()]
    [OutputType([string])]
    param([Parameter(Mandatory)] [string] $RootDir)

    if (-not (Get-SudoVdaDevice)) {
        return 'not-installed'
    }

    $nefcon = Join-Path $RootDir 'drivers\sudovda\nefconc.exe'
    if (-not (Test-Path -LiteralPath $nefcon)) {
        Write-Warning "nefconc.exe not found at $nefcon, leaving the device in place."
        return 'missing-tool'
    }

    $code = Invoke-Nefcon -NefconPath $nefcon -Arguments @(
        '--remove-device-node',
        '--hardware-id', $SudoVdaHardwareId,
        '--class-guid', $SudoVdaDisplayClass
    )
    if ($code -ne 0) {
        Write-Warning "Could not remove the virtual display driver: nefconc exit code $code"
        return 'failed'
    }

    foreach ($attempt in 1..20) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-SudoVdaDevice)) {
            Write-SudoVdaStep 'Virtual display driver removed'
            return 'removed'
        }
    }
    Write-Warning 'Could not remove the virtual display driver: the device is still present.'
    return 'failed'
}

function Open-SudoVdaLog {
    <#
    .SYNOPSIS
    Keep a transcript next to the Sunshine setup logs.

    .OUTPUTS
    The log path, or nothing if no transcript could be started.
    #>
    param([Parameter(Mandatory)] [string] $Action)
    try {
        $directory = Join-Path $env:TEMP 'Sunshine\logs\sudovda'
        $null = New-Item -ItemType Directory -Path $directory -Force
        $path = Join-Path $directory ('{0}-{1}.log' -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $Action)
        $null = Start-Transcript -LiteralPath $path -Append
        return $path
    } catch {
        return $null
    }
}

function Close-SudoVdaLog {
    param([string] $Path)
    if ($Path) {
        try {
            $null = Stop-Transcript
        } catch {
            Write-Verbose "Stop-Transcript: $($_.Exception.Message)"
        }
    }
}
