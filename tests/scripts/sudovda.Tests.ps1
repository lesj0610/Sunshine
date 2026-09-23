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
            InstanceId = "ROOT\SUDOMAKER\SUDOVDA\0000"
            Status     = $Status
        }
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
    It "reports <Expected> for processor architecture <Code>" -ForEach @(
        @{ Code = 9; Expected = "AMD64" }
        @{ Code = 12; Expected = "ARM64" }
        @{ Code = 0; Expected = "x86" }
    ) {
        Mock Get-ProcessorArchitectureCode { $Code }

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
}
