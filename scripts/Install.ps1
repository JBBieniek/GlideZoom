# Windows PowerShell 5.1. Run through Install.cmd from the extracted archive.
[CmdletBinding()]
param([switch] $ApprovedLocalTrust, [switch] $NonInteractive)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$exitCode = 1
$stage = 'preflight'
$createdDirectory = $false
$certificate = $null
$createdCertificateStores = New-Object 'System.Collections.Generic.List[string]'
$state = $null
$installDirectory = $null
$manifestPath = $null
$lifecycleMutex = $null
$ownsLifecycleMutex = $false
$registrationKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\GlideZoom'
$ownedNames = @('GlideZoom.exe', 'GlideZoom-Uninstall.exe', 'Uninstall.ps1', 'Uninstall.cmd',
    'GlideZoom-Development.cer', 'install-manifest.json', 'install-manifest.json.tmp')

function Assert-RegularPath([string] $Path, [bool] $Directory = $false) {
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Reparse points are not accepted here: $Path"
    }
    if ($item.PSIsContainer -ne $Directory) { throw "Unexpected path type: $Path" }
}

function Save-State {
    $temporaryPath = $manifestPath + '.tmp'
    Assert-RegularPath $installDirectory $true
    if (Test-Path -LiteralPath $manifestPath) { Assert-RegularPath $manifestPath }
    if (Test-Path -LiteralPath $temporaryPath) {
        Assert-RegularPath $temporaryPath
        Remove-Item -LiteralPath $temporaryPath
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($state | ConvertTo-Json -Depth 5))
    $stream = [IO.FileStream]::new($temporaryPath, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::None, 4096, [IO.FileOptions]::WriteThrough)
    try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
    if (Test-Path -LiteralPath $manifestPath) {
        # PowerShell converts ordinary $null to an empty string for this parameter.
        [IO.File]::Replace($temporaryPath, $manifestPath, [NullString]::Value)
    } else { [IO.File]::Move($temporaryPath, $manifestPath) }
}

function Assert-CreatedCertificate($Certificate) {
    if ($Certificate.Subject -cne $state.CertificateSubject -or
        $Certificate.Issuer -cne $state.CertificateSubject -or
        $Certificate.SerialNumber -ine $state.CertificateSerialNumber) {
        throw 'The certificate does not match the journaled subject, issuer, and serial.'
    }
    $eku = @($Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' })
    if ($eku.Count -ne 1) { throw 'The development certificate lacks a single EKU extension.' }
    $purposes = @($eku[0].EnhancedKeyUsages | ForEach-Object { $_.Value })
    if ($purposes.Count -ne 1 -or $purposes[0] -ne '1.3.6.1.5.5.7.3.3') {
        throw 'The generated certificate is not restricted to code signing.'
    }
}

function Remove-OwnedPrivateKey {
    if ($null -eq $state -or -not $state.CertificateCreationStarted) { return }
    $provider = [Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
    $options = [Security.Cryptography.CngKeyOpenOptions]::MachineKey
    if ([Security.Cryptography.CngKey]::Exists($state.PrivateKeyContainer, $provider, $options)) {
        $key = [Security.Cryptography.CngKey]::Open($state.PrivateKeyContainer, $provider, $options)
        try { $key.Delete() } finally { $key.Dispose() }
    }
    if ([Security.Cryptography.CngKey]::Exists($state.PrivateKeyContainer, $provider, $options)) {
        throw 'The owned private signing key still exists; recovery files must be retained.'
    }
}

function Assert-TrustedDirectoryAcl([object] $Acl) {
    if ($Acl.AreAccessRulesProtected) {
        throw 'The installation directory must inherit the Program Files permissions.'
    }
    $writeMask = [int]([Security.AccessControl.FileSystemRights]::Write) -bor
        [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [Security.AccessControl.FileSystemRights]::TakeOwnership -bor
        0x10000000 -bor 0x40000000 # GENERIC_ALL / GENERIC_WRITE may remain in raw inherited ACEs.
    $privilegedWriters = @('S-1-5-18', 'S-1-5-32-544', 'S-1-3-0',
        'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
    # Read identities as stored SIDs. Account-name translation may fail for
    # orphaned/domain/service identities even when their ACE is read-only.
    # Include inherited and inherit-only rules: they can affect the copied EXE.
    $rules = @($Acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]))
    if ($rules.Count -eq 0) {
        throw 'The installation directory has no verifiable access rules.'
    }
    foreach ($rule in $rules) {
        $sid = $rule.IdentityReference.Value
        if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            ($rule.FileSystemRights -band $writeMask) -ne 0 -and
            $privilegedWriters -notcontains $sid) {
            throw "Program Files grants write access to an unexpected principal ($sid). No UIAccess installation was completed."
        }
    }
}

function Assert-SecureInstallDirectory {
    Assert-RegularPath $installDirectory $true
    $acl = Get-Acl -LiteralPath $installDirectory
    Assert-TrustedDirectoryAcl $acl
    # Keep Program Files inheritance, and ensure the owner is the Administrators group.
    $administrators = New-Object -TypeName System.Security.Principal.SecurityIdentifier -ArgumentList 'S-1-5-32-544'
    $acl.SetOwner($administrators)
    Set-Acl -LiteralPath $installDirectory -AclObject $acl
    Assert-TrustedDirectoryAcl (Get-Acl -LiteralPath $installDirectory)
}

function Assert-OwnedFileSecurity([string] $Path) {
    Assert-RegularPath $Path
    $acl = Get-Acl -LiteralPath $Path
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin @('S-1-5-18', 'S-1-5-32-544')) {
        throw ('An installed file has an unexpected owner (' + $owner + '): ' + $Path)
    }
    Assert-TrustedDirectoryAcl $acl
}

function Remove-NewCertificate {
    $errors = New-Object 'System.Collections.Generic.List[string]'
    foreach ($store in @('My', 'Root')) {
        if ($null -eq $certificate -or -not $createdCertificateStores.Contains($store)) { continue }
        $path = 'Cert:\LocalMachine\' + $store + '\' + $certificate.Thumbprint
        try {
            if (Test-Path -LiteralPath $path) {
                $existing = Get-Item -LiteralPath $path
                if ($existing.Subject -ne $certificate.Subject -or
                    $existing.SerialNumber -ne $certificate.SerialNumber) {
                    throw "Refusing to remove a certificate whose identity changed: $path"
                }
                if ($store -eq 'My') {
                    Remove-Item -LiteralPath $path -DeleteKey -ErrorAction Stop
                } else {
                    Remove-Item -LiteralPath $path -ErrorAction Stop
                }
                if (Test-Path -LiteralPath $path) { throw "Certificate deletion did not complete: $path" }
            }
        } catch { $errors.Add($path + ': ' + $_.Exception.Message) }
    }
    try { Remove-OwnedPrivateKey } catch { $errors.Add($_.Exception.Message) }
    if ($errors.Count -gt 0) { throw ($errors -join '; ') }
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

function New-Registration {
    # RegCreateKeyEx exposes CREATED_NEW_KEY. RegistryKey.CreateSubKey alone can
    # silently open an entry that raced the absence check and must not be used.
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
public static class GlideZoomRegistrationCreate {
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
    static extern int RegCreateKeyEx(IntPtr root, string name, int reserved, string cls,
        int options, int access, IntPtr security, out SafeRegistryHandle result, out int disposition);
    public static void Create(string name, string installationId) {
        SafeRegistryHandle handle;
        int disposition;
        int code = RegCreateKeyEx(new IntPtr(unchecked((int)0x80000002)), name, 0, null,
            0, 0x2011f, IntPtr.Zero, out handle, out disposition);
        if (code != 0) throw new Win32Exception(code);
        using (handle) {
            if (disposition != 1) throw new InvalidOperationException("An Installed Apps entry already exists; nothing was overwritten.");
            using (RegistryKey key = RegistryKey.FromHandle(handle, RegistryView.Registry64)) {
                key.SetValue("InstallationId", installationId, RegistryValueKind.String);
                key.Flush();
            }
        }
    }
}
'@
    [GlideZoomRegistrationCreate]::Create($registrationKey, $state.InstallationId)
}
function Set-RegistrationValue([string] $Name, $Value) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.OpenSubKey($registrationKey, $true)
        if ($null -eq $key) { throw 'The new Installed Apps entry disappeared.' }
        try {
            Assert-OwnedRegistration (Get-Registration)
            $kind = if ($Name -in @('EstimatedSize', 'NoModify', 'NoRepair')) {
                [Microsoft.Win32.RegistryValueKind]::DWord
            } else { [Microsoft.Win32.RegistryValueKind]::String }
            $key.SetValue($Name, $Value, $kind)
            $key.Flush()
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}

try {
    if ($ApprovedLocalTrust -ne $NonInteractive) {
        throw 'Noninteractive setup requires explicit local-trust approval; both setup flags must be supplied together.'
    }
    if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
        throw 'GlideZoom requires x64 Windows and 64-bit Windows PowerShell. Run Install.cmd.'
    }
    $nativeArchitecture = $env:PROCESSOR_ARCHITEW6432
    if ([string]::IsNullOrWhiteSpace($nativeArchitecture)) {
        $nativeArchitecture = $env:PROCESSOR_ARCHITECTURE
    }
    if ($nativeArchitecture -ine 'AMD64') {
        throw "This prototype requires native x64 Windows (AMD64); detected architecture: $nativeArchitecture. ARM64 and emulated x64 are not supported."
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object -TypeName System.Security.Principal.WindowsPrincipal -ArgumentList $identity
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator permission is required. Run Install.cmd and approve its UAC prompt.'
    }
    # Child-file ownership comes from TOKEN_OWNER, not from the parent directory.
    # Reject unsupported creator-owner policies before any installation mutation;
    # do not briefly create user-owned executable/recovery files then repair them.
    if ($identity.Owner.Value -notin @('S-1-5-18', 'S-1-5-32-544')) {
        throw 'The elevated token has an unsupported default object owner. Secure installation requires Administrators or SYSTEM ownership; no files or trust were installed.'
    }
    $lifecycleMutex = [Threading.Mutex]::new($false, 'Global\GlideZoom.InstallLifecycle')
    try { $ownsLifecycleMutex = $lifecycleMutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsLifecycleMutex = $true }
    if (-not $ownsLifecycleMutex) { throw 'Another GlideZoom install or uninstall is in progress.' }
    foreach ($command in @('New-SelfSignedCertificate', 'Export-Certificate',
        'Import-Certificate', 'Set-AuthenticodeSignature', 'Get-AuthenticodeSignature')) {
        $null = Get-Command $command -ErrorAction Stop
    }

    $sourceRoot = Split-Path -Parent $PSScriptRoot
    $sourceExe = Join-Path $sourceRoot 'bin\GlideZoom.exe'
    $sourceUninstaller = Join-Path $PSScriptRoot 'Uninstall.ps1'
    $sourceUninstallerExe = Join-Path $sourceRoot 'bin\GlideZoom-Uninstall.exe'
    $sourceUninstallWrapper = Join-Path $sourceRoot 'Uninstall.cmd'
    foreach ($source in @($sourceExe, $sourceUninstaller, $sourceUninstallerExe, $sourceUninstallWrapper)) {
        Assert-RegularPath $source
    }
    $programFiles = [Environment]::GetFolderPath('ProgramFiles')
    Assert-RegularPath $programFiles $true
    $installDirectory = Join-Path $programFiles 'GlideZoom'
    $manifestPath = Join-Path $installDirectory 'install-manifest.json'
    if (Test-Path -LiteralPath $installDirectory) {
        Assert-RegularPath $installDirectory $true
        throw "An installation directory already exists. Run its Uninstall.cmd first. Nothing was overwritten: $installDirectory"
    }

    if ($null -ne (Get-Registration)) {
        throw 'An Installed Apps entry already exists. Use Settings > Apps > Installed apps to remove that installation before retrying.'
    }

    Write-Host ''
    Write-Host 'GlideZoom: LOCAL DEVELOPMENT INSTALL' -ForegroundColor Cyan
    Write-Host "Destination: $installDirectory"
    Write-Host 'This unsigned prototype needs Windows UIAccess for full-screen magnification.'
    Write-Host 'The installer will create ONE unique, local, nonexportable code-signing key;'
    Write-Host 'trust its PUBLIC certificate in the machine Root store;'
    Write-Host 'sign the installed GlideZoom.exe and GlideZoom-Uninstall.exe; then delete the private key.'
    Write-Host 'The public trust remains until uninstall. It expires in approximately one year.'
    Write-Host 'This is local development trust, not a verified publisher signature.'
    Write-Host 'No shortcuts, downloads, UAC-policy changes, or automatic startup are performed.'
    Write-Host 'Read resources\install-security.txt before proceeding.'
    Write-Host ''
    $approved = $ApprovedLocalTrust -and $NonInteractive
    if (-not $approved) { $approved = (Read-Host 'Type YES to approve this exact installation and certificate trust') -ceq 'YES' }
    if (-not $approved) {
        Write-Host 'Cancelled. Nothing was changed.'
        $exitCode = 2
    } else {
        $installationId = [Guid]::NewGuid().ToString('N')
        $subject = 'CN=GlideZoom Development ' + $installationId
        $serialNumber = '01' + [Guid]::NewGuid().ToString('N')
        $privateKeyContainer = 'GlideZoom-' + $installationId
        if ([Security.Cryptography.CngKey]::Exists($privateKeyContainer,
            [Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider,
            [Security.Cryptography.CngKeyOpenOptions]::MachineKey)) {
            throw 'The randomly named signing key already exists; nothing was overwritten.'
        }
        $exePath = Join-Path $installDirectory 'GlideZoom.exe'
        $uninstallerExePath = Join-Path $installDirectory 'GlideZoom-Uninstall.exe'
        $publicCertificatePath = Join-Path $installDirectory 'GlideZoom-Development.cer'
        # -Force is deliberately absent: a directory appearing after preflight is a failure.
        $stage = 'creating the installation directory'
        $null = New-Item -ItemType Directory -Path $installDirectory
        $createdDirectory = $true
        $stage = 'checking installation permissions'
        Assert-SecureInstallDirectory
        $stage = 'writing recovery files'
        $state = [ordered]@{
            SchemaVersion = 3
            Application = 'GlideZoom'
            InstallationId = $installationId
            Status = 'Installing'
            InstalledUtc = [DateTime]::UtcNow.ToString('o')
            InstallDirectory = $installDirectory
            Executable = $exePath
            Uninstaller = $uninstallerExePath
            RegistrationKey = $registrationKey
            RegistrationStarted = $false
            EstimatedSizeKb = 1
            UninstallScriptSha256 = (Get-FileHash -LiteralPath $sourceUninstaller -Algorithm SHA256).Hash
            SignedUninstallerSha256 = $null
            CertificateThumbprint = $null
            CertificateSubject = $subject
            CertificateSerialNumber = $serialNumber
            CertificateExpiresUtc = $null
            CertificateCreationStarted = $false
            PrivateKeyContainer = $privateKeyContainer
            OwnedCertificateStores = @()
            PrivateKeyRemoved = $false
            SourceExeSha256 = (Get-FileHash -LiteralPath $sourceExe -Algorithm SHA256).Hash
            SignedExeSha256 = $null
            OwnedFiles = $ownedNames
        }
        Save-State
        Copy-Item -LiteralPath $sourceUninstaller -Destination (Join-Path $installDirectory 'Uninstall.ps1')
        Copy-Item -LiteralPath $sourceUninstallWrapper -Destination (Join-Path $installDirectory 'Uninstall.cmd')
        Copy-Item -LiteralPath $sourceExe -Destination $exePath
        Copy-Item -LiteralPath $sourceUninstallerExe -Destination $uninstallerExePath
        foreach ($name in @('GlideZoom.exe', 'GlideZoom-Uninstall.exe', 'Uninstall.ps1', 'Uninstall.cmd', 'install-manifest.json')) {
            Assert-OwnedFileSecurity (Join-Path $installDirectory $name)
        }

        $certificateParameters = @{
            Type = 'CodeSigningCert'
            Subject = $subject
            SerialNumber = $serialNumber
            FriendlyName = 'GlideZoom local development ' + $installationId
            CertStoreLocation = 'Cert:\LocalMachine\My'
            Provider = 'Microsoft Software Key Storage Provider'
            Container = $privateKeyContainer
            KeyAlgorithm = 'RSA'
            KeyLength = 3072
            HashAlgorithm = 'SHA256'
            KeyExportPolicy = 'NonExportable'
            KeyUsage = 'DigitalSignature'
            NotBefore = (Get-Date).AddMinutes(-5)
            NotAfter = (Get-Date).AddYears(1)
        }
        # Journal intent before creating a private key. An interrupted creation must not
        # look like an empty installation that can discard its recovery manifest.
        $state.Status = 'CreatingCertificate'
        $state.CertificateCreationStarted = $true
        Save-State
        $stage = 'creating the signing certificate'
        $certificate = New-SelfSignedCertificate @certificateParameters
        $createdCertificateStores.Add('My')
        Assert-CreatedCertificate $certificate
        $state.OwnedCertificateStores = @($createdCertificateStores.ToArray())
        $state.CertificateThumbprint = $certificate.Thumbprint
        $state.CertificateSerialNumber = $certificate.SerialNumber
        $state.CertificateExpiresUtc = $certificate.NotAfter.ToUniversalTime().ToString('o')
        $state.Status = 'Installing'
        Save-State
        $stage = 'exporting the public certificate'
        $null = Export-Certificate -Cert $certificate -FilePath $publicCertificatePath -Type CERT
        foreach ($store in @('Root')) {
            $stage = 'adding the recorded Root certificate'
            $storePath = 'Cert:\LocalMachine\' + $store
            if (Test-Path -LiteralPath ($storePath + '\' + $certificate.Thumbprint)) {
                throw "Unexpected existing certificate in $store; refusing to change it."
            }
            # Record ownership only after the absence check, before a possibly partial import.
            $createdCertificateStores.Add($store)
            $state.OwnedCertificateStores = @($createdCertificateStores.ToArray())
            Save-State
            $null = Import-Certificate -FilePath $publicCertificatePath -CertStoreLocation $storePath
        }
        $stage = 'signing and verifying the two installed executables'
        foreach ($signedPath in @($exePath, $uninstallerExePath)) {
            $signature = Set-AuthenticodeSignature -FilePath $signedPath -Certificate $certificate -HashAlgorithm SHA256
            if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
                throw "Signing verification failed: $($signature.StatusMessage)"
            }
            $signature = Get-AuthenticodeSignature -LiteralPath $signedPath
            if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
                $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
                throw 'An installed executable did not pass Authenticode validation with the new certificate.'
            }
        }
        $state.SignedExeSha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
        $state.SignedUninstallerSha256 = (Get-FileHash -LiteralPath $uninstallerExePath -Algorithm SHA256).Hash
        Save-State

        # Remove both the temporary My-store entry AND its underlying CNG private key.
        $stage = 'removing the temporary signing key'
        $myCertificatePath = 'Cert:\LocalMachine\My\' + $certificate.Thumbprint
        Remove-Item -LiteralPath $myCertificatePath -DeleteKey
        if (Test-Path -LiteralPath $myCertificatePath) { throw 'The temporary certificate still exists.' }
        Remove-OwnedPrivateKey
        $state.PrivateKeyRemoved = $true
        Save-State
        foreach ($signedPath in @($exePath, $uninstallerExePath)) {
            $signature = Get-AuthenticodeSignature -LiteralPath $signedPath
            if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
                $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
                throw 'Authenticode validation failed after removal of the private signing key.'
            }
        }

        # The public certificate file is needed only to import trust. Keep the
        # recorded identity in the manifest for exact uninstall/recovery.
        $stage = 'removing the temporary public certificate file'
        Assert-RegularPath $publicCertificatePath
        Remove-Item -LiteralPath $publicCertificatePath
        if (Test-Path -LiteralPath $publicCertificatePath) {
            throw 'The temporary public certificate file could not be removed.'
        }
        $stage = 'registering Windows Installed Apps removal'
        $installedBytes = (Get-ChildItem -LiteralPath $installDirectory -Force | Measure-Object -Property Length -Sum).Sum
        $state.EstimatedSizeKb = [int][Math]::Max(1, [Math]::Ceiling($installedBytes / 1024.0))
        $state.RegistrationStarted = $true
        Save-State
        New-Registration
        foreach ($entry in (Get-RegistrationValues).GetEnumerator()) {
            Set-RegistrationValue $entry.Key $entry.Value
        }
        $completedRegistration = Get-Registration
        if ($null -eq $completedRegistration) { throw 'The Installed Apps registration disappeared before commit.' }
        Assert-OwnedRegistration $completedRegistration $true
        $stage = 'committing the completed installation'
        $state.Status = 'Installed'
        Save-State
        foreach ($name in @('GlideZoom.exe', 'GlideZoom-Uninstall.exe', 'Uninstall.ps1', 'Uninstall.cmd', 'install-manifest.json')) {
            Assert-OwnedFileSecurity (Join-Path $installDirectory $name)
        }
        Write-Host ''
        Write-Host 'Installed and signature verified. The private signing key has been deleted.' -ForegroundColor Green
        Write-Host 'Close this elevated installer, then open this file in Explorer:'
        Write-Host $exePath
        Write-Host "Uninstall: Settings > Apps > Installed apps > GlideZoom, or $uninstallerExePath"
        Write-Host "Certificate thumbprint (also in install-manifest.json): $($certificate.Thumbprint)"
        $exitCode = 0
    }
} catch {
    Write-Host ''
    Write-Host ('Installation failed (' + $stage + '): ' + $_.Exception.Message) -ForegroundColor Red
    $rollbackErrors = New-Object 'System.Collections.Generic.List[string]'
    if ($null -eq $certificate -and $null -ne $state -and $state.CertificateCreationStarted) {
        try {
            $certificateMatches = @(Get-ChildItem -LiteralPath 'Cert:\LocalMachine\My' | Where-Object {
                $_.Subject -ceq $state.CertificateSubject -and
                $_.SerialNumber -ieq $state.CertificateSerialNumber })
            if ($certificateMatches.Count -gt 1) { throw 'The journaled certificate identity is ambiguous.' }
            if ($certificateMatches.Count -eq 1) {
                Assert-CreatedCertificate $certificateMatches[0]
                $certificate = $certificateMatches[0]
                $createdCertificateStores.Add('My')
                $state.CertificateThumbprint = $certificate.Thumbprint
                $state.OwnedCertificateStores = @($createdCertificateStores.ToArray())
                Save-State
            }
        } catch { $rollbackErrors.Add('Interrupted certificate creation: ' + $_.Exception.Message) }
    }
    try { Remove-OwnedRegistration } catch {
        $rollbackErrors.Add('Installed Apps registration: ' + $_.Exception.Message)
    }
    try { Remove-NewCertificate } catch {
        $rollbackErrors.Add('Certificate/private-key cleanup: ' + $_.Exception.Message)
    }
    if ($createdDirectory -and $rollbackErrors.Count -eq 0) {
        try {
            Assert-RegularPath $installDirectory $true
            $unknown = @(Get-ChildItem -LiteralPath $installDirectory -Force |
                Where-Object { $ownedNames -notcontains $_.Name -or $_.PSIsContainer })
            if ($unknown.Count -ne 0) { throw 'Unexpected files exist; the directory has been retained.' }
            foreach ($item in @(Get-ChildItem -LiteralPath $installDirectory -Force)) {
                Assert-RegularPath $item.FullName
            }
            foreach ($name in $ownedNames) {
                $path = Join-Path $installDirectory $name
                if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
            }
            # Unlike Remove-Item, this never prompts to recurse into unexpected content.
            [IO.Directory]::Delete($installDirectory, $false)
        } catch { $rollbackErrors.Add('Installation files: ' + $_.Exception.Message) }
    }
    if ($rollbackErrors.Count -gt 0) {
        if ($null -ne $state -and (Test-Path -LiteralPath $installDirectory)) {
            try { $state.Status = 'RollbackIncomplete'; Save-State } catch {
                $rollbackErrors.Add('Could not preserve cleanup manifest: ' + $_.Exception.Message)
            }
        }
        Write-Host 'Cleanup was incomplete. Preserve the installation folder and rerun Uninstall.cmd.' -ForegroundColor Yellow
        foreach ($failure in $rollbackErrors) { Write-Host ('  ' + $failure) -ForegroundColor Yellow }
        if ($null -ne $certificate) {
            Write-Host "Certificate requiring review: $($certificate.Thumbprint) / $($certificate.Subject)"
        }
    } elseif ($createdDirectory) {
        Write-Host 'Changes made by this attempt were rolled back.'
    }
} finally {
    if ($ownsLifecycleMutex) { $lifecycleMutex.ReleaseMutex() }
    if ($null -ne $lifecycleMutex) { $lifecycleMutex.Dispose() }
}

if (-not $NonInteractive) { $null = Read-Host 'Press Enter to close this window' }
exit $exitCode
