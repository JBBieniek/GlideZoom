# Windows PowerShell 5.1. Run through Uninstall.cmd; exact installation identity is required.
[CmdletBinding()]
param([switch] $Gui, [uint32] $BootstrapProcessId = 0)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$exitCode = 1
$lifecycleMutex = $null
$ownsLifecycleMutex = $false
$shortcutPath = $null # Only a validated schema-1 installation can own a shortcut.
$registrationKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\GlideZoom'
$script:progressForm = $null
$script:progressLabel = $null
$script:progressBusy = $false
$guiReady = $false
$resultMessage = 'GlideZoom uninstallation did not complete.'

function Same-Path([string] $First, [string] $Second) {
    return [string]::Equals([IO.Path]::GetFullPath($First).TrimEnd('\'),
        [IO.Path]::GetFullPath($Second).TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
}

function Assert-RegularPath([string] $Path, [bool] $Directory = $false) {
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing a reparse point: $Path"
    }
    if ($item.PSIsContainer -ne $Directory) { throw "Unexpected path type: $Path" }
}

function Assert-OwnedFiles {
    Assert-RegularPath $installDirectory $true
    $items = @(Get-ChildItem -LiteralPath $installDirectory -Force)
    if (@($items | Where-Object { $ownedNames -notcontains $_.Name -or $_.PSIsContainer }).Count -gt 0) {
        throw 'Unexpected files exist in the installation folder. Move them elsewhere and retry; no unknown files will be deleted.'
    }
    foreach ($item in $items) { Assert-RegularPath $item.FullName }
    if ($null -ne $shortcutPath -and (Test-Path -LiteralPath $shortcutPath)) {
        Assert-RegularPath $shortcutPath
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutPath)
        if (-not (Same-Path $shortcut.TargetPath $exePath) -or $shortcut.Arguments) {
            throw 'The Start Menu shortcut was changed; refusing to delete an unrelated shortcut.'
        }
    }
}

function Save-State {
    $temporaryPath = $manifestPath + '.tmp'
    Assert-RegularPath $installDirectory $true
    Assert-RegularPath $manifestPath
    if (Test-Path -LiteralPath $temporaryPath) {
        Assert-RegularPath $temporaryPath
        Remove-Item -LiteralPath $temporaryPath
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($state | ConvertTo-Json -Depth 5))
    $stream = [IO.FileStream]::new($temporaryPath, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::None, 4096, [IO.FileOptions]::WriteThrough)
    try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
        # PowerShell converts ordinary $null to an empty string for this parameter.
        [IO.File]::Replace($temporaryPath, $manifestPath, [NullString]::Value)
}

function Assert-OwnedCertificate($Certificate) {
    if ($Certificate.Subject -cne $state.CertificateSubject -or
        $Certificate.Issuer -cne $state.CertificateSubject -or
        $Certificate.SerialNumber -ine $state.CertificateSerialNumber) {
        throw 'The certificate does not match the exact journaled identity.'
    }
    $eku = @($Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' })
    if ($eku.Count -ne 1) { throw 'The certificate has an unexpected EKU extension.' }
    $purposes = @($eku[0].EnhancedKeyUsages | ForEach-Object { $_.Value })
    if ($purposes.Count -ne 1 -or $purposes[0] -ne '1.3.6.1.5.5.7.3.3') {
        throw 'The certificate is not restricted to code signing.'
    }
}

function Remove-OwnedPrivateKey {
    if (-not $privateKeyContainer -or -not $state.CertificateCreationStarted) { return }
    $provider = [Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
    $options = [Security.Cryptography.CngKeyOpenOptions]::MachineKey
    if ([Security.Cryptography.CngKey]::Exists($privateKeyContainer, $provider, $options)) {
        $key = [Security.Cryptography.CngKey]::Open($privateKeyContainer, $provider, $options)
        try { $key.Delete() } finally { $key.Dispose() }
    }
    if ([Security.Cryptography.CngKey]::Exists($privateKeyContainer, $provider, $options)) {
        throw 'The owned private signing key still exists; recovery files must be retained.'
    }
}

function Get-OwnedProcesses([string] $Executable) {
    foreach ($process in @(Get-Process -Name GlideZoom -ErrorAction SilentlyContinue)) {
        try {
            # PowerShell can surface a failed CLR property getter as $null rather
            # than throwing here. Null is not proof that this is another app.
            $processPath = $process.Path
            if ([string]::IsNullOrWhiteSpace($processPath)) { throw 'The process path is unavailable.' }
            if (Same-Path $processPath $Executable) { $process }
        } catch {
            throw 'A GlideZoom process could not be inspected. Exit GlideZoom manually and retry.'
        }
    }
}

# Fixed 64-bit Installed Apps key. Every deletion requires the journaled owner
# marker, exact known values, and no subkeys; never delete a registry subtree.
function Get-Registration {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.OpenSubKey($registrationKey, $false)
        if ($null -eq $key) { return $null }
        try {
            $values = @{}; $kinds = @{}
            foreach ($name in $key.GetValueNames()) {
                $values[$name] = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                $kinds[$name] = $key.GetValueKind($name).ToString()
            }
            return [pscustomobject]@{ Values=$values; Kinds=$kinds; SubKeys=@($key.GetSubKeyNames()) }
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}
function Get-RegistrationValues {
    return [ordered]@{
        InstallationId = $state.InstallationId
        DisplayName = 'GlideZoom'
        DisplayVersion = '0.6.2'
        Publisher = 'GlideZoom local development build'
        InstallLocation = $installDirectory
        DisplayIcon = '"' + $exePath + '",0'
        UninstallString = '"' + (Join-Path $installDirectory 'GlideZoom-Uninstall.exe') + '"'
        EstimatedSize = [int] $state.EstimatedSizeKb
        NoModify = 1
        NoRepair = 1
    }
}
function Assert-OwnedRegistration($Registration, [bool] $RequireComplete = $false) {
    if ($null -eq $Registration) { return }
    if ($Registration.SubKeys.Count -ne 0 -or
        -not $Registration.Values.ContainsKey('InstallationId') -or
        $Registration.Values.InstallationId -cne $state.InstallationId) {
        throw 'The Installed Apps entry does not have this exact installation identity. It was retained.'
    }
    $expected = Get-RegistrationValues
    if ($RequireComplete -and $Registration.Values.Count -ne $expected.Count) {
        throw 'The Installed Apps registration is incomplete.'
    }
    foreach ($name in $Registration.Values.Keys) {
        if (-not $expected.Contains($name)) { throw 'Unexpected data exists in the Installed Apps entry. It was retained.' }
        $kind = if ($name -in @('EstimatedSize', 'NoModify', 'NoRepair')) { 'DWord' } else { 'String' }
        if ($Registration.Kinds[$name] -cne $kind -or $Registration.Values[$name] -cne $expected[$name]) {
            throw ('The Installed Apps entry was changed: ' + $name + '. It was retained.')
        }
    }
}
function Remove-OwnedRegistration {
    if ($null -eq $state -or $state.SchemaVersion -ne 3 -or -not $state.RegistrationStarted) { return }
    Assert-OwnedRegistration (Get-Registration)
    Remove-Registration
    if ($null -ne (Get-Registration)) { throw 'The Installed Apps entry could not be removed.' }
}
function Remove-Registration {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    try {
        # Recheck at the native boundary. DeleteSubKey refuses children and never recurses.
        Assert-OwnedRegistration (Get-Registration)
        $base.DeleteSubKey($registrationKey, $false)
    } finally { $base.Dispose() }
}

function Initialize-UninstallGui {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
}
function Confirm-UninstallGui {
    $message = "Remove GlideZoom from this computer?`r`n`r`nThe app, its Installed Apps entry, and only its recorded local development certificate trust will be removed.`r`n`r`nPer-user preferences and logs are kept. If GlideZoom is running, it will be asked to exit normally."
    return [Windows.Forms.MessageBox]::Show($message, 'Uninstall GlideZoom',
        [Windows.Forms.MessageBoxButtons]::YesNo, [Windows.Forms.MessageBoxIcon]::Question,
        [Windows.Forms.MessageBoxDefaultButton]::Button2) -eq [Windows.Forms.DialogResult]::Yes
}
function Start-UninstallProgress {
    $script:progressForm = New-Object Windows.Forms.Form
    $script:progressForm.Text = 'Uninstalling GlideZoom'
    $script:progressForm.ClientSize = [Drawing.Size]::new(520, 145)
    $script:progressForm.StartPosition = 'CenterScreen'
    $script:progressForm.FormBorderStyle = 'FixedDialog'
    $script:progressForm.ControlBox = $false
    $script:progressLabel = New-Object Windows.Forms.Label
    $script:progressLabel.Location = [Drawing.Point]::new(24, 24)
    $script:progressLabel.Size = [Drawing.Size]::new(472, 45)
    $script:progressLabel.Text = 'Checking the installed files and local certificate trust...'
    $bar = New-Object Windows.Forms.ProgressBar
    $bar.Location = [Drawing.Point]::new(24, 88)
    $bar.Size = [Drawing.Size]::new(472, 20)
    $bar.Style = 'Marquee'
    $script:progressForm.Controls.Add($script:progressLabel)
    $script:progressForm.Controls.Add($bar)
    $script:progressBusy = $true
    $script:progressForm.Add_FormClosing({ param($sender, $event) if ($script:progressBusy) { $event.Cancel = $true } })
    $script:progressForm.Show()
    [Windows.Forms.Application]::DoEvents()
}
function Update-UninstallProgress([string] $Message) {
    if ($Gui -and $null -ne $script:progressForm) {
        $script:progressLabel.Text = $Message
        [Windows.Forms.Application]::DoEvents()
    }
}
function Complete-UninstallGui([int] $Code, [string] $Message) {
    $script:progressBusy = $false
    if ($null -ne $script:progressForm) { $script:progressForm.Close(); $script:progressForm.Dispose() }
    if ($Code -eq 2) { return }
    $icon = if ($Code -eq 0) { [Windows.Forms.MessageBoxIcon]::Information } else { [Windows.Forms.MessageBoxIcon]::Error }
    $null = [Windows.Forms.MessageBox]::Show($Message, 'GlideZoom uninstaller',
        [Windows.Forms.MessageBoxButtons]::OK, $icon)
}
function Show-UninstallFallback([string] $Message) {
    # A failure to load Windows Forms still gets a visible native shell message.
    $shell = New-Object -ComObject WScript.Shell
    $null = $shell.Popup($Message, 0, 'GlideZoom uninstaller', 16)
}
function Wait-UninstallBootstrap {
    if ($BootstrapProcessId -eq 0) { return }
    $process = $null
    try { $process = [Diagnostics.Process]::GetProcessById([int]$BootstrapProcessId) }
    catch [ArgumentException] { return } # The small native launcher normally has already exited.
    try {
        if (-not (Same-Path $process.MainModule.FileName $uninstallerExePath)) {
            throw 'The uninstaller launcher identity changed. Close the launcher and retry.'
        }
        if (-not $process.WaitForExit(5000)) {
            throw 'The uninstaller launcher is still running. No delayed deletion was scheduled; retry after it exits.'
        }
    } finally { if ($null -ne $process) { $process.Dispose() } }
}

try {
    if ($Gui) { Initialize-UninstallGui; $guiReady = $true }
    if ($BootstrapProcessId -ne 0 -and -not $Gui) { throw 'The native launcher parameter requires GUI mode.' }
    # Cleanup remains available on a 64-bit system even if a folder was migrated from x64.
    # Only the installer enforces native AMD64; do not strand existing trust on ARM64.
    if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
        throw 'Use Uninstall.cmd with 64-bit Windows PowerShell on x64 Windows.'
    }
    $principal = New-Object -TypeName System.Security.Principal.WindowsPrincipal -ArgumentList (
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run Uninstall.cmd and approve its UAC prompt.'
    }
    $lifecycleMutex = [Threading.Mutex]::new($false, 'Global\GlideZoom.InstallLifecycle')
    try { $ownsLifecycleMutex = $lifecycleMutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsLifecycleMutex = $true }
    if (-not $ownsLifecycleMutex) { throw 'Another GlideZoom install or uninstall is in progress.' }
    $programFiles = [Environment]::GetFolderPath('ProgramFiles')
    Assert-RegularPath $programFiles $true
    $installDirectory = Join-Path $programFiles 'GlideZoom'
    $exePath = Join-Path $installDirectory 'GlideZoom.exe'
    $uninstallerExePath = Join-Path $installDirectory 'GlideZoom-Uninstall.exe'
    $manifestPath = Join-Path $installDirectory 'install-manifest.json'
    $ownedNames = @('GlideZoom.exe', 'Uninstall.ps1', 'Uninstall.cmd',
        'GlideZoom-Development.cer', 'install-manifest.json', 'install-manifest.json.tmp')

    if (-not (Test-Path -LiteralPath $installDirectory)) {
        Write-Host 'No GlideZoom installation folder was found. No certificate stores were changed.'
        Write-Host 'If you deleted the folder manually, its certificate may remain; use your saved manifest for exact cleanup.'
        $resultMessage = 'No GlideZoom installation folder was found. No certificate stores were changed.'
        $exitCode = 0
    } else {
        Assert-RegularPath $installDirectory $true
        Assert-RegularPath $manifestPath
        $state = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($state.SchemaVersion -notin @(1, 2, 3) -or $state.Application -cne 'GlideZoom' -or
            $state.InstallationId -notmatch '^[0-9a-f]{32}$' -or
            -not (Same-Path $state.InstallDirectory $installDirectory) -or
            -not (Same-Path $state.Executable $exePath) -or
            $state.Status -notin @('Installing', 'CreatingCertificate', 'Installed', 'RollbackIncomplete') -or
            $state.CertificateSerialNumber -notmatch '^[0-9A-Fa-f]+$' -or
            $state.CertificateSubject -cne ('CN=GlideZoom Development ' + $state.InstallationId)) {
            throw 'The installation manifest does not identify this exact GlideZoom installation.'
        }
        if ($state.SchemaVersion -eq 3) {
            $ownedNames = @('GlideZoom.exe', 'GlideZoom-Uninstall.exe', 'Uninstall.ps1', 'Uninstall.cmd',
                'GlideZoom-Development.cer', 'install-manifest.json', 'install-manifest.json.tmp')
            if ($state.RegistrationKey -cne $registrationKey -or
                -not (Same-Path $state.Uninstaller $uninstallerExePath) -or
                $state.RegistrationStarted -isnot [bool] -or
                $state.EstimatedSizeKb -lt 1 -or $state.EstimatedSizeKb -gt [int]::MaxValue -or
                (@($state.OwnedFiles | Sort-Object) -join '|') -cne (@($ownedNames | Sort-Object) -join '|')) {
                throw 'The Installed Apps ownership journal is invalid.'
            }
            if ($state.UninstallScriptSha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
                ($state.Status -eq 'Installed' -and $state.SignedUninstallerSha256 -notmatch '^[0-9A-Fa-f]{64}$')) {
                throw 'The uninstaller ownership hashes are invalid.'
            }
            if ($state.RegistrationStarted) { Assert-OwnedRegistration (Get-Registration) }
            foreach ($entry in @(
                @{ Path=(Join-Path $installDirectory 'Uninstall.ps1'); Hash=$state.UninstallScriptSha256 },
                @{ Path=$uninstallerExePath; Hash=$state.SignedUninstallerSha256 })) {
                # Incomplete installs may not yet have signed the new uninstaller.
                if ($entry.Hash -and (Test-Path -LiteralPath $entry.Path) -and
                    (Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash -cne $entry.Hash) {
                    throw 'An installed uninstaller file changed. Its files and journal were retained.'
                }
            }
        }
        Assert-OwnedFiles
        Wait-UninstallBootstrap
        $certificateStores = @('My', 'Root')
        if ($state.SchemaVersion -eq 1) {
            # Old installs own one exact Start Menu shortcut and may also own
            # TrustedPublisher trust. New installs never inspect either one.
            $commonPrograms = [Environment]::GetFolderPath('CommonPrograms')
            Assert-RegularPath $commonPrograms $true
            $expectedShortcut = Join-Path $commonPrograms 'GlideZoom.lnk'
            if ($state.PSObject.Properties.Name -notcontains 'StartMenuShortcut' -or
                -not (Same-Path $state.StartMenuShortcut $expectedShortcut)) {
                throw 'The legacy installation manifest has an unexpected shortcut path.'
            }
            $shortcutPath = $expectedShortcut
            $certificateStores = @('My', 'TrustedPublisher', 'Root')
            Assert-OwnedFiles
        } elseif ($state.PSObject.Properties.Name -contains 'StartMenuShortcut' -and
                  $null -ne $state.StartMenuShortcut) {
            throw 'This installation schema must not record a shortcut.'
        }
        $thumbprint = $state.CertificateThumbprint
        if ($thumbprint -and $thumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
            throw 'The recorded certificate identity is invalid.'
        }
        if ($state.Status -eq 'Installed' -and -not $thumbprint) {
            throw 'A completed installation has no certificate identity. No changes were made.'
        }
        $ownedStores = @($state.OwnedCertificateStores)
        if (@($ownedStores | Where-Object { $_ -notin $certificateStores }).Count -ne 0) {
            throw 'The manifest records an unexpected certificate store.'
        }
        if (-not $thumbprint -and $ownedStores.Count -ne 0) {
            throw 'Certificate stores were recorded without a certificate identity.'
        }
        $privateKeyContainer = $null
        if ($state.PSObject.Properties.Name -contains 'PrivateKeyContainer') {
            $privateKeyContainer = $state.PrivateKeyContainer
            if ($privateKeyContainer -cne ('GlideZoom-' + $state.InstallationId)) {
                throw 'The recorded private-key container does not belong to this installation.'
            }
        }
        $recoveredIdentity = $false
        if (-not $thumbprint -and $state.CertificateCreationStarted) {
            $certificateMatches = @(Get-ChildItem -LiteralPath 'Cert:\LocalMachine\My' | Where-Object {
                $_.Subject -ceq $state.CertificateSubject -and
                $_.SerialNumber -ieq $state.CertificateSerialNumber })
            if ($certificateMatches.Count -gt 1) { throw 'The journaled certificate identity is ambiguous; recovery files were retained.' }
            if ($certificateMatches.Count -eq 1) {
                Assert-OwnedCertificate $certificateMatches[0]
                $thumbprint = $certificateMatches[0].Thumbprint
                $ownedStores = @('My')
                $state.CertificateThumbprint = $thumbprint
                $state.OwnedCertificateStores = $ownedStores
                $recoveredIdentity = $true
            } elseif (-not $privateKeyContainer) {
                throw 'The interrupted older installation has no exact private-key container identity. Preserve its manifest for manual certificate-store review.'
            }
        }
        # Validate every certificate before removing any of them. Never search by a broad name.
        $certificatePaths = @()
        if ($thumbprint) {
            foreach ($store in $certificateStores) {
                if ($ownedStores -notcontains $store) { continue }
                $path = 'Cert:\LocalMachine\' + $store + '\' + $thumbprint
                if (Test-Path -LiteralPath $path) {
                    $cert = Get-Item -LiteralPath $path
                    Assert-OwnedCertificate $cert
                    $certificatePaths += [pscustomobject]@{ Path = $path; Store = $store }
                }
            }
        }
        Write-Host 'GlideZoom uninstaller' -ForegroundColor Cyan
        Write-Host "Remove the app and its exact local development trust: $thumbprint"
        if ($null -ne $shortcutPath) { Write-Host "Also remove its legacy shortcut: $shortcutPath" }
        $approved = if ($Gui) { Confirm-UninstallGui } else { (Read-Host 'Type YES to uninstall') -ceq 'YES' }
        if (-not $approved) {
            Write-Host 'Cancelled. Nothing was changed.'
            $exitCode = 2
        } else {
            if ($Gui) { Start-UninstallProgress }
            # Recheck after the prompt. Preserve recovered identity before any deletion.
            Assert-OwnedFiles
            if ($recoveredIdentity) { Save-State }
            Update-UninstallProgress 'Asking GlideZoom to exit normally...'
            $running = @(Get-OwnedProcesses $exePath)
            if ($running.Count -gt 0) {
                if (-not $state.SignedExeSha256 -or
                    (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash -ne $state.SignedExeSha256) {
                    throw 'The executable differs from the installed copy. Exit GlideZoom manually before uninstalling.'
                }
                # On Windows, -Verb Open selects ShellExecute. Do not add redirection or -NoNewWindow.
                $quitProcess = Start-Process -FilePath $exePath -ArgumentList '--quit' -Verb Open -PassThru
                if ($null -ne $quitProcess -and -not $quitProcess.WaitForExit(5000)) {
                    throw 'The quit request did not complete. Exit GlideZoom from its tray menu and retry.'
                }
                for ($attempt = 0; $attempt -lt 20; $attempt++) {
                    if (@(Get-OwnedProcesses $exePath).Count -eq 0) { break }
                    Start-Sleep -Milliseconds 250
                }
                if (@(Get-OwnedProcesses $exePath).Count -ne 0) {
                    throw 'GlideZoom is still running. Exit it from the tray menu and retry; no process was force-terminated.'
                }
            }

            Update-UninstallProgress 'Removing only the recorded local certificate and private key...'
            $failures = New-Object 'System.Collections.Generic.List[string]'
            Assert-OwnedFiles
            foreach ($entry in $certificatePaths) {
                try {
                    if (-not (Test-Path -LiteralPath $entry.Path)) { continue }
                    Assert-OwnedCertificate (Get-Item -LiteralPath $entry.Path)
                    if ($entry.Store -eq 'My') {
                        Remove-Item -LiteralPath $entry.Path -DeleteKey -ErrorAction Stop
                    } else {
                        Remove-Item -LiteralPath $entry.Path -ErrorAction Stop
                    }
                    if (Test-Path -LiteralPath $entry.Path) { throw 'Certificate deletion did not complete.' }
                } catch { $failures.Add($entry.Path + ': ' + $_.Exception.Message) }
            }
            try { Remove-OwnedPrivateKey } catch { $failures.Add('Private signing key: ' + $_.Exception.Message) }
            if ($null -ne $shortcutPath -and (Test-Path -LiteralPath $shortcutPath)) {
                try { Remove-Item -LiteralPath $shortcutPath -Force } catch {
                    $failures.Add('Start Menu shortcut: ' + $_.Exception.Message)
                }
            }
            if ($failures.Count -gt 0) {
                throw ('Trust or shortcut cleanup was incomplete. Files and manifest were retained. ' +
                    ($failures -join '; '))
            }
            Assert-OwnedFiles
            Update-UninstallProgress 'Removing the installed app and its Installed Apps entry...'
            # Retain the manifest and uninstall entry points until other deletions have succeeded.
            foreach ($name in @('GlideZoom.exe', 'GlideZoom-Development.cer', 'install-manifest.json.tmp')) {
                $path = Join-Path $installDirectory $name
                if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
            }
            Remove-OwnedRegistration
            # The small launcher has exited. No reboot task, wildcard, or delayed delete is used.
            if ($state.SchemaVersion -eq 3 -and (Test-Path -LiteralPath $uninstallerExePath)) {
                Remove-Item -LiteralPath $uninstallerExePath -Force
            }
            foreach ($name in @('Uninstall.cmd', 'Uninstall.ps1', 'install-manifest.json')) {
                $path = Join-Path $installDirectory $name
                if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
            }
            # An unexpected new file makes removal fail; it is never recursively erased.
            [IO.Directory]::Delete($installDirectory, $false)
            Write-Host 'GlideZoom and its local development certificate trust were removed.' -ForegroundColor Green
            Write-Host 'Per-user preferences and logs, if any, are not removed.'
            $resultMessage = 'GlideZoom and its local development certificate trust were removed. Per-user preferences and logs were kept.'
            $exitCode = 0
        }
    }
} catch {
    $resultMessage = 'Uninstallation did not complete: ' + $_.Exception.Message + "`r`n`r`nKeep the installation folder and its manifest, then retry. No process was force-terminated and no unknown files were recursively deleted."
    Write-Host ('Uninstallation did not complete: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'No unrelated certificates or recursively discovered files are removed by this script.'
    Write-Host 'Keep the installation manifest until cleanup is complete.'
} finally {
    if ($ownsLifecycleMutex) { $lifecycleMutex.ReleaseMutex() }
    if ($null -ne $lifecycleMutex) { $lifecycleMutex.Dispose() }
}

if ($Gui -and $guiReady) { Complete-UninstallGui $exitCode $resultMessage }
elseif (-not $Gui) { $null = Read-Host 'Press Enter to close this window' }
else {
    # If Forms could not load, provide a final visible failure through the Windows shell.
    # No installation paths or certificate stores are changed by this fallback.
    Show-UninstallFallback $resultMessage
}
exit $exitCode
