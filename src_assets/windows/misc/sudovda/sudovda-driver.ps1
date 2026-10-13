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

# How Windows knows the device. nefconc creates the device node from the
# class name, so Windows names the instance ROOT\DISPLAY\nnnn like any other
# root-enumerated display adapter. Only the hardware ID says it is SudoVDA.
$SudoVdaHardwareId   = 'root\sudomaker\sudovda'
$SudoVdaDisplayClass = '{4D36E968-E325-11CE-BFC1-08002BE10318}'

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

# ERROR_SUCCESS_REBOOT_REQUIRED. nefconc exits with it when the change was made
# but only completes after a restart.
$SudoVdaRebootRequired = 3010

function Write-SudoVdaStep {
    # To the host, not the information stream: Windows PowerShell 5.1 leaves
    # Write-Information out of transcripts, and the install log is one. It
    # would keep the warnings and lose every step and the result.
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '',
        Justification = 'Windows PowerShell 5.1 transcripts leave out Write-Information')]
    param([string] $Message)
    Write-Host "  $Message"
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
        Where-Object { $_.HardwareID -contains $SudoVdaHardwareId }
}

function Test-SudoVdaDeviceReady {
    <#
    .SYNOPSIS
    Whether a SudoVDA device node has the driver installed on it and running.

    .DESCRIPTION
    A node can outlive its driver. A removal that takes the driver but not the
    node leaves one with the hardware ID and no driver, no class and no device
    interface, and Windows still reports it as OK. The status alone does not
    say the driver is there; the INF the node was installed from does.
    #>
    [OutputType([bool])]
    param([Parameter(Mandatory)] $Device)

    if ($Device.Status -ne 'OK') {
        return $false
    }
    $inf = Get-PnpDeviceProperty -InstanceId $Device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' `
        -ErrorAction SilentlyContinue
    return [bool] ($inf -and $inf.Data)
}

function Wait-SudoVdaDevice {
    <#
    .SYNOPSIS
    Wait for the device to come up with its driver after the driver install.

    .OUTPUTS
    The first device that is up, else the device as last seen, or nothing if
    none appeared.
    #>
    $device = $null
    foreach ($attempt in 1..20) {
        Start-Sleep -Milliseconds 500
        $devices = @(Get-SudoVdaDevice)
        foreach ($candidate in $devices) {
            if (Test-SudoVdaDeviceReady -Device $candidate) {
                return $candidate
            }
        }
        if ($devices.Count -gt 0) {
            $device = $devices[0]
        }
    }
    return $device
}

function Wait-SudoVdaDeviceGone {
    <#
    .SYNOPSIS
    Wait for every SudoVDA device node to go.

    .OUTPUTS
    True once none is left.
    #>
    [OutputType([bool])]
    param()

    foreach ($attempt in 1..20) {
        Start-Sleep -Milliseconds 500
        if (-not (Get-SudoVdaDevice)) {
            return $true
        }
    }
    return $false
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

function Invoke-SudoVdaTool {
    <#
    .SYNOPSIS
    Run a tool, log what it says under its name, and return its exit code.
    #>
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string[]] $Arguments
    )

    # The tools may write to stderr. That is output to log, not an error to
    # stop on, which is what Windows PowerShell makes of it under 'Stop'.
    $ErrorActionPreference = 'Continue'
    $output = & $Path @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    foreach ($line in $output) {
        $text = "$line".Trim()
        if ($text) {
            Write-SudoVdaStep "  ${Name}: $text"
        }
    }
    return $exitCode
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
    return Invoke-SudoVdaTool -Path $NefconPath -Name 'nefconc' -Arguments $Arguments
}

function Get-PnputilPath {
    <#
    .SYNOPSIS
    Where this process finds pnputil.

    .DESCRIPTION
    pnputil is only in the 64-bit System32. A 32-bit PowerShell is redirected
    from there to SysWOW64, and reaches the real one through Sysnative.
    #>
    [OutputType([string])]
    param()

    $directory = 'System32'
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        $directory = 'Sysnative'
    }
    return Join-Path $env:SystemRoot "$directory\pnputil.exe"
}

function Invoke-Pnputil {
    <#
    .SYNOPSIS
    Run pnputil, log what it says, and return its exit code, or -1 if it is
    not there.
    #>
    param([Parameter(Mandatory)] [string[]] $Arguments)

    $pnputil = Get-PnputilPath
    if (-not (Test-Path -LiteralPath $pnputil)) {
        Write-Warning "pnputil.exe not found at $pnputil"
        return -1
    }
    return Invoke-SudoVdaTool -Path $pnputil -Name 'pnputil' -Arguments $Arguments
}

function Get-NefconOutcome {
    <#
    .SYNOPSIS
    What a nefconc exit code means: success, reboot-required or failure.

    .DESCRIPTION
    Only 0 and 3010 are success. 3010 means the change was made and completes
    after a restart: it must not be undone, and the device may not show it yet.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [int] $ExitCode)

    if ($ExitCode -eq 0) {
        return 'success'
    }
    if ($ExitCode -eq $SudoVdaRebootRequired) {
        return 'reboot-required'
    }
    return 'failure'
}

function Uninstall-SudoVdaDeviceNode {
    <#
    .SYNOPSIS
    Remove one device node by its instance ID, whatever state it is in.

    .DESCRIPTION
    nefconc looks devices up through their class, so it cannot see a node
    that lost its class along with its driver. pnputil removes the instance
    itself, and exits 0 or 3010 as nefconc does.

    .OUTPUTS
    success, reboot-required or failure.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [string] $InstanceId)

    $code = Invoke-Pnputil -Arguments @('/remove-device', $InstanceId)
    return Get-NefconOutcome -ExitCode $code
}

function Uninstall-SudoVdaDevice {
    <#
    .SYNOPSIS
    Remove every SudoVDA device node, and the driver package with them.

    .DESCRIPTION
    nefconc removes the devices it can find and then the driver package,
    unless something else still uses it. A node it cannot find, because it
    lost its class along with its driver, is then removed by its instance ID.

    .OUTPUTS
    removed, removed-reboot-required or failed.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [string] $NefconPath)

    $code = Invoke-Nefcon -NefconPath $NefconPath -Arguments @(
        '--remove-device-node',
        '--hardware-id', $SudoVdaHardwareId,
        '--class-guid', $SudoVdaDisplayClass
    )
    $outcome = Get-NefconOutcome -ExitCode $code
    if ($outcome -eq 'failure') {
        Write-Warning "nefconc could not remove the virtual display driver (exit code $code)"
        return 'failed'
    }
    if ($outcome -eq 'reboot-required') {
        # Removed, finishing at the next restart; the device may be listed
        # until then.
        return 'removed-reboot-required'
    }
    if (Wait-SudoVdaDeviceGone) {
        return 'removed'
    }

    $rebootRequired = $false
    foreach ($device in @(Get-SudoVdaDevice)) {
        Write-SudoVdaStep "Removing $($device.InstanceId), which nefconc could not see"
        $outcome = Uninstall-SudoVdaDeviceNode -InstanceId $device.InstanceId
        if ($outcome -eq 'failure') {
            Write-Warning "Could not remove the device node $($device.InstanceId)"
            return 'failed'
        }
        $rebootRequired = $rebootRequired -or ($outcome -eq 'reboot-required')
    }
    if ($rebootRequired) {
        return 'removed-reboot-required'
    }
    if (Wait-SudoVdaDeviceGone) {
        return 'removed'
    }
    Write-Warning 'The virtual display device is still present.'
    return 'failed'
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
                    $outcome = Uninstall-SudoVdaDevice -NefconPath $NefconPath
                    if ($outcome -eq 'failed') {
                        throw 'the device node is still there'
                    }
                    if ($outcome -eq 'removed-reboot-required') {
                        Write-SudoVdaStep "  $($step.Description) goes after a restart"
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
    What happened: installed, installed-reboot-required, already-installed,
    skipped-no-files, skipped-architecture, not-asked, declined or failed.
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

    # A node with the hardware ID is only the driver if the driver is on it.
    # One that is not, left by a removal that took the driver and not the
    # node, would otherwise pass for an install forever and keep the driver
    # from ever being installed again.
    $brokenNodes = @(Get-SudoVdaDevice)
    foreach ($device in $brokenNodes) {
        if (Test-SudoVdaDeviceReady -Device $device) {
            Write-SudoVdaStep 'SudoVDA is already installed, leaving it as it is.'
            return 'already-installed'
        }
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

        # --- broken nodes ---
        # Replaced rather than repaired, so the install below is the same one
        # a machine that never had the driver gets. There is nothing to put
        # back if a later step fails: the node was of no use.
        foreach ($device in $brokenNodes) {
            Write-SudoVdaStep "Removing $($device.InstanceId), which has the SudoVDA hardware ID but not its driver"
            $outcome = Uninstall-SudoVdaDeviceNode -InstanceId $device.InstanceId
            if ($outcome -eq 'failure') {
                throw "could not remove the device node $($device.InstanceId)"
            }
            if ($outcome -eq 'reboot-required') {
                Write-SudoVdaStep "  $($device.InstanceId) goes after a restart"
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
        # A step that needs a restart has still succeeded, so it is kept, and
        # the device cannot be expected to be up until after that restart.
        $rebootRequired = $false

        $code = Invoke-Nefcon -NefconPath $nefcon -Arguments @(
            '--create-device-node',
            '--class-name', 'Display',
            '--class-guid', $SudoVdaDisplayClass,
            '--hardware-id', $SudoVdaHardwareId
        )
        $outcome = Get-NefconOutcome -ExitCode $code
        if ($outcome -eq 'failure') {
            throw "could not create the device node (nefconc exit code $code)"
        }
        $rebootRequired = $rebootRequired -or ($outcome -eq 'reboot-required')
        [void] $undo.Add([pscustomobject]@{
            Kind        = 'device-node'
            Store       = $null
            Description = 'device node'
        })

        $code = Invoke-Nefcon -NefconPath $nefcon -Arguments @('--install-driver', '--inf-path', $inf)
        $outcome = Get-NefconOutcome -ExitCode $code
        if ($outcome -eq 'failure') {
            throw "could not install the driver (nefconc exit code $code)"
        }
        $rebootRequired = $rebootRequired -or ($outcome -eq 'reboot-required')

        if ($rebootRequired) {
            Write-Warning 'The virtual display driver is installed and needs a restart to finish.'
            return 'installed-reboot-required'
        }

        # --- verify ---
        $device = Wait-SudoVdaDevice
        if (-not $device) {
            throw 'the driver installed but no device appeared'
        }
        if (-not (Test-SudoVdaDeviceReady -Device $device)) {
            throw "the device is present but its driver is not running on it (status '$($device.Status)')"
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
    else still uses it, and a node it cannot see goes by its instance ID. The
    signing certificate stays in the trusted stores on purpose: other software
    may have been installed under the same certificate, and removing it would
    break their signature checks.

    .OUTPUTS
    What happened: removed, removed-reboot-required, not-installed,
    missing-tool or failed.
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

    $result = Uninstall-SudoVdaDevice -NefconPath $nefcon
    switch ($result) {
        'removed' {
            Write-SudoVdaStep 'Virtual display driver removed'
        }
        'removed-reboot-required' {
            Write-Warning 'The virtual display driver is removed and needs a restart to finish.'
        }
        default {
            Write-Warning 'Could not remove the virtual display driver.'
        }
    }
    return $result
}

function Get-SudoVdaExitCode {
    <#
    .SYNOPSIS
    The exit code for a result: 0, 3010 when a restart finishes the job, or 1
    when it did not happen.

    .DESCRIPTION
    Declining, and a machine the driver does not support, are not failures.
    not-asked is: the caller asked for an install that could not go ahead
    without consent, and should pass -Silent if it has that consent. Missing
    files or a missing nefconc mean an incomplete package.
    #>
    [OutputType([int])]
    param([Parameter(Mandatory)] [string] $Result)

    switch ($Result) {
        'installed'                 { return 0 }
        'already-installed'         { return 0 }
        'declined'                  { return 0 }
        'skipped-architecture'      { return 0 }
        'removed'                   { return 0 }
        'not-installed'             { return 0 }
        'installed-reboot-required' { return $SudoVdaRebootRequired }
        'removed-reboot-required'   { return $SudoVdaRebootRequired }
    }
    return 1
}

function Invoke-SudoVdaInstall {
    <#
    .SYNOPSIS
    What install-sudovda.ps1 does: install, log, and return the exit code.
    #>
    [OutputType([int])]
    param(
        [Parameter(Mandatory)] [string] $RootDir,
        [switch] $Silent
    )

    $log = Open-SudoVdaLog -Action 'install'
    try {
        $result = Install-SudoVdaDriver -RootDir $RootDir -Silent:$Silent
        Write-SudoVdaStep "Result: $result"
        return Get-SudoVdaExitCode -Result $result
    } catch {
        Write-Warning "Virtual display driver install failed: $($_.Exception.Message)"
        return 1
    } finally {
        Close-SudoVdaLog -Path $log
    }
}

function Invoke-SudoVdaUninstall {
    <#
    .SYNOPSIS
    What uninstall-sudovda.ps1 does: remove, log, and return the exit code.
    #>
    [OutputType([int])]
    param([Parameter(Mandatory)] [string] $RootDir)

    $log = Open-SudoVdaLog -Action 'uninstall'
    try {
        $result = Uninstall-SudoVdaDriver -RootDir $RootDir
        Write-SudoVdaStep "Result: $result"
        return Get-SudoVdaExitCode -Result $result
    } catch {
        Write-Warning "Could not remove the virtual display driver: $($_.Exception.Message)"
        return 1
    } finally {
        Close-SudoVdaLog -Path $log
    }
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
