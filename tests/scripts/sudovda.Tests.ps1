BeforeAll {
    $sourcePath = Join-Path `
        $PSScriptRoot `
        "..\..\src_assets\windows\misc\sudovda\sudovda-driver.ps1"
    $script:shippedDriverDir = Join-Path `
        $PSScriptRoot `
        "..\..\src_assets\windows\drivers\sudovda"

    # Windows-only commands the functions call. Stand-ins where they do not
    # exist, so there is something to mock; on Windows the real ones are used.
    if (-not (Get-Command Get-PnpDevice -ErrorAction SilentlyContinue)) {
        function Get-PnpDevice {
            [CmdletBinding()]
            param()
        }
    }
    if (-not (Get-Command Import-Certificate -ErrorAction SilentlyContinue)) {
        function Import-Certificate {
            [CmdletBinding()]
            param([string]$FilePath, [string]$CertStoreLocation)
            $null = $FilePath, $CertStoreLocation
        }
    }

    . $sourcePath

    function Get-FakeCertificate {
        param([string]$Store, [string]$Thumbprint)
        [PSCustomObject]@{
            Thumbprint = $Thumbprint
            PSPath     = "Cert:\LocalMachine\$Store\$Thumbprint"
        }
    }

    function Get-FakeDevice {
        param([string]$Status = "OK")
        [PSCustomObject]@{
            InstanceId = "ROOT\DISPLAY\0001"
            HardwareID = @("ROOT\SUDOMAKER\SUDOVDA")
            Status     = $Status
        }
    }
}

Describe "Get-SudoVdaDevice" {
    It "finds the device by its hardware ID, whatever Windows named the instance" {
        Mock Get-PnpDevice {
            @(
                [PSCustomObject]@{ InstanceId = "ROOT\DISPLAY\0000"; HardwareID = @("ROOT\OTHERVENDOR\IDD"); Status = "OK" }
                Get-FakeDevice
                [PSCustomObject]@{ InstanceId = "PCI\VEN_10DE&DEV_1234\3&0"; HardwareID = @("PCI\VEN_10DE&DEV_1234"); Status = "OK" }
            )
        }

        $found = @(Get-SudoVdaDevice)

        $found.Count | Should -Be 1
        $found[0].InstanceId | Should -Be "ROOT\DISPLAY\0001"
    }

    It "finds nothing when no device has the hardware ID" {
        Mock Get-PnpDevice {
            [PSCustomObject]@{ InstanceId = "ROOT\DISPLAY\0000"; HardwareID = @("ROOT\OTHERVENDOR\IDD"); Status = "OK" }
        }

        @(Get-SudoVdaDevice).Count | Should -Be 0
    }
}

Describe "SudoVDA certificate thumbprint" {
    It "is the shipped certificate's thumbprint, which is what the stores report" {
        $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
            (Join-Path $script:shippedDriverDir "sudovda.cer"))

        $SudoVdaCertificateThumbprint | Should -Be $certificate.Thumbprint
        $certificate.Subject | Should -Be "CN=sudovda@su.mk"
    }
}

Describe "Get-SudoVdaCertificate" {
    It "finds the certificate by its SHA-1 thumbprint" {
        Mock Get-ChildItem {
            @(
                Get-FakeCertificate -Store "Root" -Thumbprint "0000000000000000000000000000000000000000"
                Get-FakeCertificate -Store "Root" -Thumbprint "3C918FC73525AD8B1521B6DB26B71F694277CC49"
            )
        } -ParameterFilter { $Path -eq "Cert:\LocalMachine\Root" }

        $found = @(Get-SudoVdaCertificate -Store "Root")

        $found.Count | Should -Be 1
        $found[0].Thumbprint | Should -Be "3C918FC73525AD8B1521B6DB26B71F694277CC49"
    }

    It "does not take the certificate file's SHA-256 for a thumbprint" {
        Mock Get-ChildItem {
            Get-FakeCertificate `
                -Store "Root" `
                -Thumbprint "6ACCDCD519F6179D967DB4EAA20ECF25A732BA30E87F4CFFEBC768B2C13C9007"
        } -ParameterFilter { $Path -eq "Cert:\LocalMachine\Root" }

        @(Get-SudoVdaCertificate -Store "Root").Count | Should -Be 0
    }
}

Describe "Remove-SudoVdaCertificate" {
    It "removes only the SudoVDA certificate" {
        Mock Get-ChildItem {
            @(
                Get-FakeCertificate -Store "TrustedPublisher" -Thumbprint "1111111111111111111111111111111111111111"
                Get-FakeCertificate -Store "TrustedPublisher" -Thumbprint "3C918FC73525AD8B1521B6DB26B71F694277CC49"
            )
        } -ParameterFilter { $Path -eq "Cert:\LocalMachine\TrustedPublisher" }
        Mock Remove-Item {}

        Remove-SudoVdaCertificate -Store "TrustedPublisher"

        Should -Invoke -CommandName Remove-Item -Times 1 -Exactly -Scope It
        Should -Invoke -CommandName Remove-Item -Times 1 -Exactly -Scope It -ParameterFilter {
            $LiteralPath -like "*3C918FC73525AD8B1521B6DB26B71F694277CC49"
        }
    }
}

Describe "Test-SudoVdaPayload" {
    It "accepts the driver files as shipped" {
        { Test-SudoVdaPayload -DriverDir $script:shippedDriverDir } | Should -Not -Throw
    }

    It "rejects a file that was changed" {
        $copy = Join-Path $TestDrive "tampered"
        Copy-Item -LiteralPath $script:shippedDriverDir -Destination $copy -Recurse
        Add-Content -LiteralPath (Join-Path $copy "SudoVDA.inf") -Value "; changed"

        { Test-SudoVdaPayload -DriverDir $copy } | Should -Throw "*SudoVDA.inf*"
    }

    It "rejects a missing file" {
        $copy = Join-Path $TestDrive "incomplete"
        Copy-Item -LiteralPath $script:shippedDriverDir -Destination $copy -Recurse
        Remove-Item -LiteralPath (Join-Path $copy "nefconc.exe")

        { Test-SudoVdaPayload -DriverDir $copy } | Should -Throw "*nefconc.exe*"
    }
}

Describe "Get-NativeArchitecture" {
    It "reports <Expected> for processor architecture <ProcessorCode>" -ForEach @(
        @{ ProcessorCode = 9; Expected = "AMD64" }
        @{ ProcessorCode = 12; Expected = "ARM64" }
        @{ ProcessorCode = 0; Expected = "x86" }
    ) {
        Mock Get-ProcessorArchitectureCode { $ProcessorCode }

        Get-NativeArchitecture | Should -Be $Expected
    }

    Context "without WMI" {
        BeforeEach {
            $script:savedArchitecture = $env:PROCESSOR_ARCHITECTURE
            $script:savedWow = $env:PROCESSOR_ARCHITEW6432
            Mock Get-ProcessorArchitectureCode { throw "no WMI here" }
        }

        AfterEach {
            $env:PROCESSOR_ARCHITECTURE = $script:savedArchitecture
            $env:PROCESSOR_ARCHITEW6432 = $script:savedWow
        }

        It "falls back to the process environment" {
            $env:PROCESSOR_ARCHITECTURE = "ARM64"
            $env:PROCESSOR_ARCHITEW6432 = $null

            Get-NativeArchitecture | Should -Be "ARM64"
        }

        It "prefers the native architecture a 32-bit process is told about" {
            $env:PROCESSOR_ARCHITECTURE = "x86"
            $env:PROCESSOR_ARCHITEW6432 = "AMD64"

            Get-NativeArchitecture | Should -Be "AMD64"
        }
    }
}

Describe "Invoke-Nefcon" {
    It "returns the exit code and logs stdout and stderr without stopping on stderr" {
        $ErrorActionPreference = "Stop"
        $script:logged = [System.Collections.Generic.List[string]]::new()
        Mock Write-SudoVdaStep { $script:logged.Add($Message) }
        $shell = (Get-Process -Id $PID).Path

        $code = Invoke-Nefcon -NefconPath $shell -Arguments @(
            "-NoProfile",
            "-Command",
            "[Console]::Out.WriteLine('to stdout'); [Console]::Error.WriteLine('to stderr'); exit 3"
        )

        $code | Should -Be 3
        $script:logged | Should -Contain "  nefconc: to stdout"
        $script:logged | Should -Contain "  nefconc: to stderr"
    }
}

Describe "Install-SudoVdaDriver" {
    BeforeEach {
        $root = Join-Path $TestDrive ([guid]::NewGuid().ToString())
        New-Item -ItemType Directory -Path (Join-Path $root "drivers\sudovda") -Force | Out-Null

        Mock Get-NativeArchitecture { "AMD64" }
        Mock Get-SudoVdaDevice {}
        Mock Test-SudoVdaPayload {}
        Mock Test-CanPrompt { $false }
        Mock Read-Host { "y" }
        Mock Write-SudoVdaConsentText {}
        Mock Get-SudoVdaCertificate {}
        Mock Add-SudoVdaCertificate {}
        Mock Remove-SudoVdaCertificate {}
        Mock Invoke-Nefcon { 0 }
        Mock Wait-SudoVdaDevice { Get-FakeDevice }
        Mock Write-Warning {}
        Mock Write-SudoVdaStep {}
    }

    It "installs without asking when told to" {
        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "installed"

        Should -Invoke -CommandName Read-Host -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 1 -Exactly -Scope It -ParameterFilter { $Store -eq "Root" }
        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 1 -Exactly -Scope It -ParameterFilter { $Store -eq "TrustedPublisher" }
        Should -Invoke -CommandName Invoke-Nefcon -Times 1 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--create-device-node" }
        Should -Invoke -CommandName Invoke-Nefcon -Times 1 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--install-driver" }
    }

    It "does not add a certificate that is already trusted" {
        Mock Get-SudoVdaCertificate { Get-FakeCertificate -Store $Store -Thumbprint $SudoVdaCertificateThumbprint }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "installed"

        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 0 -Exactly -Scope It
    }

    It "removes the certificates it added when a later step fails" {
        Mock Invoke-Nefcon { 1 } -ParameterFilter { $Arguments -contains "--install-driver" }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "failed"

        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 1 -Exactly -Scope It -ParameterFilter { $Store -eq "Root" }
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 1 -Exactly -Scope It -ParameterFilter { $Store -eq "TrustedPublisher" }
        Should -Invoke -CommandName Invoke-Nefcon -Times 1 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--remove-device-node" }
    }

    It "keeps a certificate it did not add, even when it rolls back" {
        Mock Get-SudoVdaCertificate {
            Get-FakeCertificate -Store "Root" -Thumbprint $SudoVdaCertificateThumbprint
        } -ParameterFilter { $Store -eq "Root" }
        Mock Invoke-Nefcon { 1 } -ParameterFilter { $Arguments -contains "--install-driver" }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "failed"

        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 0 -Exactly -Scope It -ParameterFilter { $Store -eq "Root" }
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 1 -Exactly -Scope It -ParameterFilter { $Store -eq "TrustedPublisher" }
    }

    It "undoes the certificates when the device never comes up" {
        Mock Wait-SudoVdaDevice {}

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "failed"

        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 2 -Exactly -Scope It
    }

    It "changes nothing on <Architecture>" -ForEach @(
        @{ Architecture = "ARM64" }
        @{ Architecture = "x86" }
    ) {
        Mock Get-NativeArchitecture { $Architecture }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "skipped-architecture"

        Should -Invoke -CommandName Test-SudoVdaPayload -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It
    }

    It "does not install when nobody can be asked and it was not told to" {
        Install-SudoVdaDriver -RootDir $root | Should -Be "not-asked"

        Should -Invoke -CommandName Read-Host -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It
    }

    It "asks in an interactive session and changes nothing when declined" {
        Mock Test-CanPrompt { $true }
        Mock Read-Host { "n" }

        Install-SudoVdaDriver -RootDir $root | Should -Be "declined"

        Should -Invoke -CommandName Write-SudoVdaConsentText -Times 1 -Exactly -Scope It
        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It
    }

    It "installs after an interactive yes" {
        Mock Test-CanPrompt { $true }
        Mock Read-Host { "" }

        Install-SudoVdaDriver -RootDir $root | Should -Be "installed"

        Should -Invoke -CommandName Read-Host -Times 1 -Exactly -Scope It
    }

    It "leaves an installed driver alone" {
        Mock Get-SudoVdaDevice { Get-FakeDevice }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "already-installed"

        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It
    }

    It "skips when the driver files are not there" {
        $empty = Join-Path $TestDrive "no-driver"
        New-Item -ItemType Directory -Path $empty -Force | Out-Null

        Install-SudoVdaDriver -RootDir $empty -Silent | Should -Be "skipped-no-files"

        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 0 -Exactly -Scope It
    }

    It "stops before trusting anything when a file fails verification" {
        Mock Test-SudoVdaPayload { throw "SudoVDA.dll does not match its expected SHA-256" }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "failed"

        Should -Invoke -CommandName Add-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It
    }

    It "keeps a device node that needs a restart, and does not wait for the device" {
        Mock Invoke-Nefcon { 3010 } -ParameterFilter { $Arguments -contains "--create-device-node" }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "installed-reboot-required"

        Should -Invoke -CommandName Invoke-Nefcon -Times 1 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--install-driver" }
        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--remove-device-node" }
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Wait-SudoVdaDevice -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Write-Warning -Times 0 -Exactly -Scope It -ParameterFilter { $Message -like "*failed*" }
    }

    It "keeps a driver install that needs a restart, and does not wait for the device" {
        Mock Invoke-Nefcon { 3010 } -ParameterFilter { $Arguments -contains "--install-driver" }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "installed-reboot-required"

        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--remove-device-node" }
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Wait-SudoVdaDevice -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Write-Warning -Times 0 -Exactly -Scope It -ParameterFilter { $Message -like "*failed*" }
    }

    It "counts a rollback removal that needs a restart as undone and keeps going" {
        Mock Get-SudoVdaCertificate {
            Get-FakeCertificate -Store "Root" -Thumbprint $SudoVdaCertificateThumbprint
        } -ParameterFilter { $Store -eq "Root" }
        Mock Invoke-Nefcon { 1 } -ParameterFilter { $Arguments -contains "--install-driver" }
        Mock Invoke-Nefcon { 3010 } -ParameterFilter { $Arguments -contains "--remove-device-node" }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "failed"

        Should -Invoke -CommandName Invoke-Nefcon -Times 1 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--remove-device-node" }
        Should -Invoke -CommandName Write-Warning -Times 0 -Exactly -Scope It -ParameterFilter { $Message -like "*could not undo*" }
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 1 -Exactly -Scope It -ParameterFilter { $Store -eq "TrustedPublisher" }
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 0 -Exactly -Scope It -ParameterFilter { $Store -eq "Root" }
    }

    It "treats nefconc exit code <NefconExit> as a failure" -ForEach @(
        @{ NefconExit = 1 }
        @{ NefconExit = 3011 }
        @{ NefconExit = -1 }
    ) {
        Mock Invoke-Nefcon { $NefconExit } -ParameterFilter { $Arguments -contains "--install-driver" }

        Install-SudoVdaDriver -RootDir $root -Silent | Should -Be "failed"

        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 2 -Exactly -Scope It
    }
}

Describe "Uninstall-SudoVdaDriver" {
    BeforeEach {
        $root = Join-Path $TestDrive ([guid]::NewGuid().ToString())
        $driverDir = Join-Path $root "drivers\sudovda"
        New-Item -ItemType Directory -Path $driverDir -Force | Out-Null
        New-Item -ItemType File -Path (Join-Path $driverDir "nefconc.exe") | Out-Null

        $script:deviceChecks = 0
        Mock Get-SudoVdaDevice {
            $script:deviceChecks++
            if ($script:deviceChecks -eq 1) {
                Get-FakeDevice
            }
        }
        Mock Invoke-Nefcon { 0 }
        Mock Remove-SudoVdaCertificate {}
        Mock Start-Sleep {}
        Mock Write-Warning {}
        Mock Write-SudoVdaStep {}
    }

    It "removes the device and keeps the certificate" {
        Uninstall-SudoVdaDriver -RootDir $root | Should -Be "removed"

        Should -Invoke -CommandName Invoke-Nefcon -Times 1 -Exactly -Scope It -ParameterFilter { $Arguments -contains "--remove-device-node" }
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 0 -Exactly -Scope It
    }

    It "does nothing when the driver is not installed" {
        Mock Get-SudoVdaDevice {}

        Uninstall-SudoVdaDriver -RootDir $root | Should -Be "not-installed"

        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It
    }

    It "reports a removal nefconc refused" {
        Mock Invoke-Nefcon { 5 }

        Uninstall-SudoVdaDriver -RootDir $root | Should -Be "failed"
    }

    It "reports a device that stays" {
        Mock Get-SudoVdaDevice { Get-FakeDevice }

        Uninstall-SudoVdaDriver -RootDir $root | Should -Be "failed"
    }

    It "leaves the device when nefconc is missing" {
        Remove-Item -LiteralPath (Join-Path $root "drivers\sudovda\nefconc.exe")

        Uninstall-SudoVdaDriver -RootDir $root | Should -Be "missing-tool"

        Should -Invoke -CommandName Invoke-Nefcon -Times 0 -Exactly -Scope It
    }

    It "reports a removal that needs a restart without waiting for the device to go" {
        Mock Invoke-Nefcon { 3010 }

        Uninstall-SudoVdaDriver -RootDir $root | Should -Be "removed-reboot-required"

        Should -Invoke -CommandName Start-Sleep -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Get-SudoVdaDevice -Times 1 -Exactly -Scope It
        Should -Invoke -CommandName Remove-SudoVdaCertificate -Times 0 -Exactly -Scope It
        Should -Invoke -CommandName Write-Warning -Times 0 -Exactly -Scope It -ParameterFilter { $Message -like "Could not*" }
    }
}

Describe "Get-NefconOutcome" {
    It "reads <NefconExit> as <Expected>" -ForEach @(
        @{ NefconExit = 0; Expected = "success" }
        @{ NefconExit = 3010; Expected = "reboot-required" }
        @{ NefconExit = 1; Expected = "failure" }
        @{ NefconExit = 5; Expected = "failure" }
        @{ NefconExit = 3011; Expected = "failure" }
        @{ NefconExit = -1; Expected = "failure" }
    ) {
        Get-NefconOutcome -ExitCode $NefconExit | Should -Be $Expected
    }
}

Describe "Get-SudoVdaExitCode" {
    It "exits <Expected> for <Result>" -ForEach @(
        @{ Result = "installed"; Expected = 0 }
        @{ Result = "already-installed"; Expected = 0 }
        @{ Result = "declined"; Expected = 0 }
        @{ Result = "skipped-architecture"; Expected = 0 }
        @{ Result = "removed"; Expected = 0 }
        @{ Result = "not-installed"; Expected = 0 }
        @{ Result = "installed-reboot-required"; Expected = 3010 }
        @{ Result = "removed-reboot-required"; Expected = 3010 }
        @{ Result = "failed"; Expected = 1 }
        @{ Result = "not-asked"; Expected = 1 }
        @{ Result = "skipped-no-files"; Expected = 1 }
        @{ Result = "missing-tool"; Expected = 1 }
    ) {
        Get-SudoVdaExitCode -Result $Result | Should -Be $Expected
    }
}

Describe "Invoke-SudoVdaInstall" {
    BeforeEach {
        Mock Open-SudoVdaLog {}
        Mock Close-SudoVdaLog {}
        Mock Write-SudoVdaStep {}
        Mock Write-Warning {}
    }

    It "returns <Expected> for <Result>" -ForEach @(
        @{ Result = "installed"; Expected = 0 }
        @{ Result = "installed-reboot-required"; Expected = 3010 }
        @{ Result = "failed"; Expected = 1 }
    ) {
        Mock Install-SudoVdaDriver { $Result }

        Invoke-SudoVdaInstall -RootDir $TestDrive -Silent | Should -Be $Expected

        Should -Invoke -CommandName Install-SudoVdaDriver -Times 1 -Exactly -Scope It -ParameterFilter { $Silent }
        Should -Invoke -CommandName Close-SudoVdaLog -Times 1 -Exactly -Scope It
    }

    It "returns a failure when the install throws" {
        Mock Install-SudoVdaDriver { throw "unexpected" }

        Invoke-SudoVdaInstall -RootDir $TestDrive | Should -Be 1

        Should -Invoke -CommandName Close-SudoVdaLog -Times 1 -Exactly -Scope It
    }
}

Describe "Invoke-SudoVdaUninstall" {
    BeforeEach {
        Mock Open-SudoVdaLog {}
        Mock Close-SudoVdaLog {}
        Mock Write-SudoVdaStep {}
        Mock Write-Warning {}
    }

    It "returns <Expected> for <Result>" -ForEach @(
        @{ Result = "removed"; Expected = 0 }
        @{ Result = "removed-reboot-required"; Expected = 3010 }
        @{ Result = "failed"; Expected = 1 }
    ) {
        Mock Uninstall-SudoVdaDriver { $Result }

        Invoke-SudoVdaUninstall -RootDir $TestDrive | Should -Be $Expected
    }

    It "returns a failure when the removal throws" {
        Mock Uninstall-SudoVdaDriver { throw "unexpected" }

        Invoke-SudoVdaUninstall -RootDir $TestDrive | Should -Be 1
    }
}

Describe "install-sudovda.ps1" {
    It "passes its result on as the process exit code" {
        $scripts = Join-Path $TestDrive "Sunshine\scripts"
        New-Item -ItemType Directory -Path $scripts -Force | Out-Null
        $source = Split-Path -Parent $sourcePath
        foreach ($name in @("install-sudovda.ps1", "sudovda-driver.ps1")) {
            Copy-Item -LiteralPath (Join-Path $source $name) -Destination $scripts
        }
        $shell = (Get-Process -Id $PID).Path

        # No drivers folder next to it: an incomplete package, which is a failure.
        & $shell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts "install-sudovda.ps1") -Silent *> $null

        $LASTEXITCODE | Should -Be 1
    }
}

Describe "SudoVDA batch wrappers" {
    # The last hop of the exit code contract: install-sudovda.ps1 exits with
    # what Invoke-SudoVdaInstall returned, powershell.exe exits with that, and
    # the batch file passes %errorlevel% on. A stand-in sudovda-driver.ps1 makes
    # the result 3010 without a driver. Windows only: cmd.exe and powershell.exe
    # are what is being tested, and a Unix exit status would truncate 3010.
    BeforeEach {
        $scripts = Join-Path $TestDrive ("scripts-" + [guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $scripts -Force | Out-Null
        $source = Split-Path -Parent $sourcePath
        foreach ($name in @(
            "install-sudovda.ps1",
            "uninstall-sudovda.ps1",
            "install-sudovda.bat",
            "uninstall-sudovda.bat"
        )) {
            Copy-Item -LiteralPath (Join-Path $source $name) -Destination $scripts
        }
        Set-Content -LiteralPath (Join-Path $scripts "sudovda-driver.ps1") -Value @(
            'function Invoke-SudoVdaInstall { param([string] $RootDir, [switch] $Silent) $null = $RootDir, $Silent; return 3010 }'
            'function Invoke-SudoVdaUninstall { param([string] $RootDir) $null = $RootDir; return 3010 }'
        )

        function Invoke-BatchFile {
            param([string] $Path, [string] $Arguments = "")
            $process = Start-Process `
                -FilePath "cmd.exe" `
                -ArgumentList "/d /s /c `"`"$Path`" $Arguments`"" `
                -NoNewWindow `
                -PassThru `
                -RedirectStandardOutput (Join-Path $TestDrive "batch-stdout.txt") `
                -RedirectStandardError (Join-Path $TestDrive "batch-stderr.txt")
            $null = $process.Handle
            $process.WaitForExit()
            return $process.ExitCode
        }
    }

    It "install-sudovda.bat exits 3010 when the install needs a restart" -Skip:([Environment]::OSVersion.Platform -ne "Win32NT") {
        Invoke-BatchFile -Path (Join-Path $scripts "install-sudovda.bat") -Arguments "-Silent" | Should -Be 3010
    }

    It "uninstall-sudovda.bat exits 3010 when the removal needs a restart" -Skip:([Environment]::OSVersion.Platform -ne "Win32NT") {
        Invoke-BatchFile -Path (Join-Path $scripts "uninstall-sudovda.bat") | Should -Be 3010
    }
}
