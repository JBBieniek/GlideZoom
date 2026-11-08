# Executes production installer/uninstaller control flow with MOCKED Windows APIs.
# No elevation, certificate provider, Windows ACL, process, or real app mutex is used.
# Only fixture files below one unique temporary directory are created or removed.
[CmdletBinding()]
param([string] $ReportPath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('GlideZoomLifecycle-' + [Guid]::NewGuid().ToString('N'))
$contextName = 'GlideZoomLifecycleTestContext'
if (Get-Variable -Name $contextName -Scope Global -ErrorAction SilentlyContinue) {
    throw 'A lifecycle test context is already active.'
}
$oldArchitecture = $env:PROCESSOR_ARCHITECTURE
$oldWowArchitecture = $env:PROCESSOR_ARCHITEW6432
$results = New-Object 'System.Collections.Generic.List[string]'
$caseRecords = New-Object 'System.Collections.Generic.List[object]'
$runHashes = [ordered]@{}
foreach ($relative in @('scripts/Install.ps1', 'scripts/Uninstall.ps1', 'tests/install_lifecycle_tests.ps1')) {
    # These fixed audit inputs intentionally live outside the mutable fixture.
    # Use the real hash module's stream API so its WinPS5 implementation does not
    # route a read-only integrity check through the fixture's Test-Path mock.
    $sourceStream = [IO.File]::OpenRead((Join-Path $project $relative))
    try { $runHashes[$relative] = (Microsoft.PowerShell.Utility\Get-FileHash -InputStream $sourceStream -Algorithm SHA256).Hash.ToLowerInvariant() }
    finally { $sourceStream.Dispose() }
}

function Get-TestContext { return $global:GlideZoomLifecycleTestContext }
function Assert-Test([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw ('ASSERTION: ' + $Message) }
}
function Hit-Boundary([string] $Name) {
    $context = Get-TestContext
    if (-not $context.Hits.ContainsKey($Name)) { $context.Hits[$Name] = 0 }
    $context.Hits[$Name]++
    if ($context.Failure -ceq $Name) { throw ('INJECTED: ' + $Name) }
}
function Assert-Reached([string] $Name) {
    Assert-Test ((Get-TestContext).Hits.ContainsKey($Name)) ('Boundary was not reached: ' + $Name)
}
function Assert-FixturePath([string] $Path) {
    $context = Get-TestContext
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $context.Root + [IO.Path]::DirectorySeparatorChar
    if ($full -cne $context.Root -and -not $full.StartsWith($prefix, [StringComparison]::Ordinal)) {
        throw ('Test attempted filesystem access outside its fixture: ' + $Path)
    }
}
function Resolve-TestCertificatePath([string] $Path) {
    if ($Path -notmatch '^Cert:\\LocalMachine\\(My|Root|TrustedPublisher)(?:\\([0-9a-fA-F]{40}))?$') {
        throw ('Unexpected certificate-provider path in test: ' + $Path)
    }
    $thumbprint = $null
    if ($Matches.ContainsKey(2)) { $thumbprint = $Matches[2].ToUpperInvariant() }
    return [pscustomobject]@{ Store = $Matches[1]; Thumbprint = $thumbprint }
}
function Get-TestFolder([string] $Name) {
    Hit-Boundary ('Folder:' + $Name)
    $context = Get-TestContext
    switch ($Name) {
        'ProgramFiles' { return $context.ProgramFiles }
        'CommonPrograms' { return $context.CommonPrograms }
        default { throw ('Unmocked special folder: ' + $Name) }
    }
}
function Get-TestIdentity { Hit-Boundary 'Identity'; return [pscustomobject]@{ TestIdentity = $true; Owner=[pscustomobject]@{ Value=(Get-TestContext).TokenOwner } } }
function Get-TestMutex {
    Hit-Boundary 'MutexCreate'
    $mutex = [pscustomobject]@{}
    $mutex | Add-Member ScriptMethod WaitOne {
        param($Timeout)
        Hit-Boundary 'MutexWait'
        return (Get-TestContext).MutexAvailable
    }
    $mutex | Add-Member ScriptMethod ReleaseMutex { Hit-Boundary 'MutexRelease' }
    $mutex | Add-Member ScriptMethod Dispose { Hit-Boundary 'MutexDispose' }
    return $mutex
}
function Test-TestCngKey([string] $KeyName) {
    Hit-Boundary 'KeyExists'
    return (Get-TestContext).Keys.ContainsKey($KeyName)
}
function Open-TestCngKey([string] $KeyName) {
    Hit-Boundary 'KeyOpen'
    $key = [pscustomobject]@{ Name = $KeyName }
    $key | Add-Member ScriptMethod Delete {
        Hit-Boundary 'DeleteKey'
        (Get-TestContext).Keys.Remove($this.Name)
    }
    $key | Add-Member ScriptMethod Dispose {}
    return $key
}
function Set-TestExit([int] $Code) { (Get-TestContext).ExitCode = $Code }

function Convert-TestScript([string] $Source, [string] $Destination) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Source, [ref] $tokens, [ref] $errors)
    Assert-Test ($errors.Count -eq 0) ('Production script did not parse: ' + $Source)
    $edits = New-Object 'System.Collections.Generic.List[object]'
    foreach ($node in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.InvokeMemberExpressionAst] }, $true)) {
        if (-not $node.Static) { continue }
        $type = $node.Expression.Extent.Text
        $member = $node.Member.Value
        $replacement = $null
        if ($type -ceq '[Security.Principal.WindowsIdentity]' -and $member -ceq 'GetCurrent') {
            $replacement = '(Get-TestIdentity)'
        } elseif ($type -ceq '[Environment]' -and $member -ceq 'GetFolderPath') {
            Assert-Test ($node.Arguments.Count -eq 1) 'Unexpected GetFolderPath argument count'
            $replacement = '(Get-TestFolder ' + $node.Arguments[0].Extent.Text + ')'
        } elseif ($type -ceq '[Threading.Mutex]' -and $member -ceq 'new') {
            Assert-Test ($node.Extent.Text -match 'Global\\GlideZoom\.InstallLifecycle') 'Unexpected production mutex'
            $replacement = '(Get-TestMutex)'
        } elseif ($type -ceq '[Security.Cryptography.CngKey]' -and $member -in @('Exists', 'Open')) {
            Assert-Test ($node.Arguments.Count -eq 3) 'Unexpected CNG argument count'
            $command = if ($member -ceq 'Exists') { 'Test-TestCngKey' } else { 'Open-TestCngKey' }
            $replacement = '(' + $command + ' -KeyName (' + $node.Arguments[0].Extent.Text + '))'
        }
        if ($null -ne $replacement) {
            $edits.Add([pscustomobject]@{ Start = $node.Extent.StartOffset; End = $node.Extent.EndOffset; Text = $replacement })
        }
    }
    $nativeFunctions = @{
        'Get-Registration' = 'Get-TestRegistration'
        'New-Registration' = 'New-TestRegistration'
        'Set-RegistrationValue' = 'param([string] $Name, $Value) Set-TestRegistrationValue $Name $Value'
        'Remove-Registration' = 'Remove-TestRegistration'
        'Initialize-UninstallGui' = "Hit-Boundary 'GuiInitialize'"
        'Confirm-UninstallGui' = "Hit-Boundary 'GuiConfirm'; return (Get-TestContext).GuiAnswer"
        'Start-UninstallProgress' = 'Hit-Boundary ''GuiProgressStart''; (Get-TestContext).GuiOpen = $true'
        'Update-UninstallProgress' = 'param([string] $Message) if ($Gui) { Hit-Boundary ''GuiProgressUpdate'' }'
        'Complete-UninstallGui' = 'param([int] $Code, [string] $Message) Hit-Boundary ''GuiComplete''; (Get-TestContext).GuiOpen = $false; (Get-TestContext).GuiResult = $Code'
        'Show-UninstallFallback' = 'param([string] $Message) Hit-Boundary ''GuiFallback'''
        'Wait-UninstallBootstrap' = 'if ($BootstrapProcessId -ne 0) { Hit-Boundary ''BootstrapWait''; if ((Get-TestContext).BootstrapBusy) { throw ''Injected bootstrap still running'' } }'
    }
    foreach ($node in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
        if ($nativeFunctions.ContainsKey($node.Name)) {
            $replacement = 'function ' + $node.Name + ' { ' + $nativeFunctions[$node.Name] + ' }'
            $edits.Add([pscustomobject]@{ Start=$node.Extent.StartOffset; End=$node.Extent.EndOffset; Text=$replacement })
        }
    }
    foreach ($node in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.ExitStatementAst] }, $true)) {
        Assert-Test ($node.Pipeline.Extent.Text -ceq '$exitCode') 'Unexpected production exit expression'
        $edits.Add([pscustomobject]@{ Start = $node.Extent.StartOffset; End = $node.Extent.EndOffset; Text = 'Set-TestExit $exitCode; return' })
    }
    Assert-Test ($edits.Count -ge 7) 'Native boundary rewrite did not find expected operations'
    $text = [IO.File]::ReadAllText($Source)
    foreach ($edit in ($edits | Sort-Object Start -Descending)) {
        $text = $text.Remove($edit.Start, $edit.End - $edit.Start).Insert($edit.Start, $edit.Text)
    }
    Assert-Test ($text -notmatch '\[Security\.Principal\.WindowsIdentity\]::GetCurrent') 'Identity call escaped isolation'
    Assert-Test ($text -notmatch '\[Security\.Cryptography\.CngKey\]::(Exists|Open)') 'CNG call escaped isolation'
    Assert-Test ($text -notmatch '\[Microsoft\.Win32\.RegistryKey\]::') 'Registry call escaped isolation'
    Assert-Test ($text -notmatch 'Add-Type') 'Native Add-Type call escaped isolation'
    Assert-Test ($text -notmatch 'Global\\GlideZoom\.InstallLifecycle') 'Real lifecycle mutex escaped isolation'
    [IO.File]::WriteAllText($Destination, $text)
}

function Get-TestRegistration {
    Hit-Boundary 'ReadRegistration'
    $entry = (Get-TestContext).Registration
    if ($null -eq $entry) { return $null }
    return [pscustomobject]@{ Values=$entry.Values.Clone(); Kinds=$entry.Kinds.Clone(); SubKeys=@($entry.SubKeys) }
}
function New-TestRegistration {
    Hit-Boundary 'CreateRegistration'
    $context = Get-TestContext
    Assert-Test ($registrationKey -ceq 'Software\Microsoft\Windows\CurrentVersion\Uninstall\GlideZoom') 'Unexpected registry key'
    if ($null -ne $context.Registration) { throw 'Mock existing registry key: atomic create refuses overwrite' }
    $context.Registration = [pscustomobject]@{ Values=@{}; Kinds=@{}; SubKeys=@() }
    Hit-Boundary 'CreateRegistrationBeforeMarker'
    $context.Registration.Values.InstallationId = $state.InstallationId
    $context.Registration.Kinds.InstallationId = 'String'
    Hit-Boundary 'CreateRegistrationAfterMarker'
}
function Set-TestRegistrationValue([string] $Name, $Value) {
    Hit-Boundary ('WriteRegistration:' + $Name)
    Assert-OwnedRegistration (Get-TestRegistration)
    $entry = (Get-TestContext).Registration
    $entry.Values[$Name] = $Value
    $entry.Kinds[$Name] = if ($Name -in @('EstimatedSize', 'NoModify', 'NoRepair')) { 'DWord' } else { 'String' }
    Hit-Boundary 'WriteRegistrationAfterMutation'
}
function Remove-TestRegistration {
    Hit-Boundary 'RemoveRegistration'
    Assert-OwnedRegistration (Get-TestRegistration)
    (Get-TestContext).Registration = $null
}

# Every Windows command used by the scripts is intercepted even when these tests
# run on Windows. File operations delegate only after checking their fixture root.
function New-Object {
    [CmdletBinding()]
    param([Parameter(Position=0)][string] $TypeName, [object[]] $ArgumentList, [string] $ComObject)
    if ($TypeName -ceq 'System.Security.Principal.WindowsPrincipal') {
        $value = [pscustomobject]@{}
        $value | Add-Member ScriptMethod IsInRole { param($Role) return (Get-TestContext).IsAdministrator }
        return $value
    }
    if ($TypeName -ceq 'System.Security.Principal.SecurityIdentifier') {
        return [pscustomobject]@{ Value = [string] $ArgumentList[0] }
    }
    if ($ComObject) {
        Assert-Test ($ComObject -ceq 'WScript.Shell') 'Unexpected COM object'
        Hit-Boundary 'LegacyShortcutOpen'
        $value = [pscustomobject]@{}
        $value | Add-Member ScriptMethod CreateShortcut {
            param($Path)
            Assert-FixturePath $Path
            return (Get-TestContext).Shortcut
        }
        return $value
    }
    Microsoft.PowerShell.Utility\New-Object @PSBoundParameters
}
function New-TestRule([string] $Sid, [Security.AccessControl.FileSystemRights] $Rights) {
    $identity = [pscustomobject]@{ Value = $Sid }
    $identity | Add-Member ScriptMethod Translate { throw 'Account-name translation must never be requested.' }
    return [pscustomobject]@{
        IdentityReference = $identity; FileSystemRights = $Rights
        AccessControlType = [Security.AccessControl.AccessControlType]::Allow
        InheritanceFlags = [Security.AccessControl.InheritanceFlags]::None
        PropagationFlags = [Security.AccessControl.PropagationFlags]::None
        IsInherited = $true
    }
}
function Get-Acl {
    [CmdletBinding()]
    param([string] $LiteralPath)
    Assert-FixturePath $LiteralPath
    Hit-Boundary 'GetAcl'
    $context = Get-TestContext
    $rights = [Security.AccessControl.FileSystemRights]::ReadAndExecute
    if ($context.UnsafeAcl) { $rights = [Security.AccessControl.FileSystemRights]::Write }
    $acl = [pscustomobject]@{
        AreAccessRulesProtected = $false
        Owner = if ([IO.Path]::GetFileName($LiteralPath) -ceq $context.BadFileOwnerName) { 'S-1-5-21-111-222-333-1001' } else { 'S-1-5-32-544' }
        Rules = @(
            (New-TestRule 'S-1-5-18' ([Security.AccessControl.FileSystemRights]::FullControl)),
            (New-TestRule 'S-1-5-32-544' ([Security.AccessControl.FileSystemRights]::FullControl)),
            (New-TestRule 'S-1-15-3-999999-222222-333333' $rights)
        )
    }
    $acl | Add-Member ScriptProperty Access { throw 'Name-based ACL access must never be requested.' }
    $acl | Add-Member ScriptMethod GetAccessRules {
        param($Explicit, $Inherited, $TargetType)
        Assert-Test ($Explicit -and $Inherited -and $TargetType -eq [Security.Principal.SecurityIdentifier]) 'ACL must request all raw SID rules'
        Hit-Boundary 'RawSidRules'
        return $this.Rules
    }
    $acl | Add-Member ScriptMethod GetOwner { param($TargetType) Assert-Test ($TargetType -eq [Security.Principal.SecurityIdentifier]) 'File owner must be read as raw SID'; Hit-Boundary 'FileOwnerRead'; return [pscustomobject]@{ Value=$this.Owner } }
    $acl | Add-Member ScriptMethod SetOwner { param($Owner) Assert-Test ($Owner.Value -ceq 'S-1-5-32-544') 'Unexpected directory owner' }
    return $acl
}
function Set-Acl {
    [CmdletBinding()]
    param([string] $LiteralPath, $AclObject)
    Assert-FixturePath $LiteralPath
    Hit-Boundary 'SetAcl'
}
function Test-Path {
    [CmdletBinding()]
    param([string] $LiteralPath, [string] $Path,
        [ValidateSet('Any', 'Container', 'Leaf')] [string] $PathType = 'Any')
    $value = if ($PSBoundParameters.ContainsKey('LiteralPath')) { $LiteralPath } else { $Path }
    if ($value -like 'Cert:*') {
        $entry = Resolve-TestCertificatePath $value
        if (-not $entry.Thumbprint) { return $PathType -ne 'Leaf' }
        return $PathType -ne 'Container' -and (Get-TestContext).Stores[$entry.Store].ContainsKey($entry.Thumbprint)
    }
    Assert-FixturePath $value
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
function Get-Item {
    [CmdletBinding()]
    param([string] $LiteralPath, [switch] $Force)
    if ($LiteralPath -like 'Cert:*') {
        $entry = Resolve-TestCertificatePath $LiteralPath
        return (Get-TestContext).Stores[$entry.Store][$entry.Thumbprint]
    }
    Assert-FixturePath $LiteralPath
    Microsoft.PowerShell.Management\Get-Item @PSBoundParameters
}
function Get-ChildItem {
    [CmdletBinding()]
    param([string] $LiteralPath, [switch] $Force)
    if ($LiteralPath -like 'Cert:*') {
        $entry = Resolve-TestCertificatePath $LiteralPath
        return @((Get-TestContext).Stores[$entry.Store].Values)
    }
    Assert-FixturePath $LiteralPath
    Microsoft.PowerShell.Management\Get-ChildItem @PSBoundParameters
}
function New-Item {
    [CmdletBinding()]
    param([string] $ItemType, [string] $Path)
    Assert-FixturePath $Path
    Microsoft.PowerShell.Management\New-Item @PSBoundParameters
}
function Get-FileHash {
    [CmdletBinding()]
    param([string] $LiteralPath, [string] $Algorithm)
    Assert-FixturePath $LiteralPath
    Microsoft.PowerShell.Utility\Get-FileHash @PSBoundParameters
}
function Get-Content {
    [CmdletBinding()]
    param([string] $LiteralPath, [switch] $Raw, [string] $Encoding)
    Assert-FixturePath $LiteralPath
    Microsoft.PowerShell.Management\Get-Content @PSBoundParameters
}
function Copy-Item {
    [CmdletBinding()]
    param([string] $LiteralPath, [string] $Destination)
    Assert-FixturePath $LiteralPath; Assert-FixturePath $Destination
    if ([IO.Path]::GetFileName($Destination) -ceq 'GlideZoom.exe') { Hit-Boundary 'CopyExecutable' }
    if ([IO.Path]::GetFileName($Destination) -ceq 'GlideZoom-Uninstall.exe') { Hit-Boundary 'CopyUninstaller' }
    Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
}
function Remove-Item {
    [CmdletBinding()]
    param([string] $LiteralPath, [switch] $Force, [switch] $DeleteKey)
    if ($LiteralPath -like 'Cert:*') {
        $context = Get-TestContext
        $entry = Resolve-TestCertificatePath $LiteralPath
        Hit-Boundary ('RemoveCertificate:' + $entry.Store)
        $certificate = $context.Stores[$entry.Store][$entry.Thumbprint]
        $context.Stores[$entry.Store].Remove($entry.Thumbprint)
        if ($DeleteKey) {
            Hit-Boundary 'DeleteKey'
            $context.Keys.Remove($certificate.KeyName)
        }
        return
    }
    Assert-FixturePath $LiteralPath
    if ([IO.Path]::GetFileName($LiteralPath) -ceq 'GlideZoom.exe') { Hit-Boundary 'RemoveExecutable' }
    if ([IO.Path]::GetFileName($LiteralPath) -ceq 'GlideZoom-Uninstall.exe') { Hit-Boundary 'RemoveUninstaller' }
    Microsoft.PowerShell.Management\Remove-Item @PSBoundParameters
}
function New-SelfSignedCertificate {
    [CmdletBinding()]
    param($Type, $Subject, $SerialNumber, $FriendlyName, $CertStoreLocation,
        $Provider, $Container, $KeyAlgorithm, $KeyLength, $HashAlgorithm,
        $KeyExportPolicy, $KeyUsage, $NotBefore, $NotAfter)
    Hit-Boundary 'CreateCertificate'
    Assert-Test ($CertStoreLocation -ceq 'Cert:\LocalMachine\My') 'Unexpected creation store'
    Assert-Test ($Type -ceq 'CodeSigningCert' -and $KeyExportPolicy -ceq 'NonExportable') 'Unexpected certificate policy'
    Assert-Test ($Provider -ceq 'Microsoft Software Key Storage Provider') 'Unexpected key provider'
    $context = Get-TestContext
    $context.Keys[$Container] = $true
    $certificate = [pscustomobject]@{
        Subject=$Subject; Issuer=$Subject; SerialNumber=$SerialNumber
        Thumbprint=('A' * 40); NotAfter=$NotAfter; KeyName=$Container
        Extensions=@([pscustomobject]@{
            Oid=[pscustomobject]@{ Value='2.5.29.37' }
            EnhancedKeyUsages=@([pscustomobject]@{ Value='1.3.6.1.5.5.7.3.3' })
        })
    }
    $context.Stores.My[$certificate.Thumbprint] = $certificate
    Hit-Boundary 'CreateCertificateAfterMutation'
    return $certificate
}
function Export-Certificate {
    [CmdletBinding()]
    param($Cert, [string] $FilePath, $Type)
    Assert-FixturePath $FilePath
    Hit-Boundary 'ExportCertificate'
    [IO.File]::WriteAllText($FilePath, $Cert.Thumbprint)
}
function Import-Certificate {
    [CmdletBinding()]
    param([string] $FilePath, [string] $CertStoreLocation)
    Assert-FixturePath $FilePath
    Hit-Boundary 'ImportCertificate'
    $entry = Resolve-TestCertificatePath $CertStoreLocation
    Assert-Test ($entry.Store -ceq 'Root') 'New installation must import Root only'
    $context = Get-TestContext
    $thumbprint = [IO.File]::ReadAllText($FilePath)
    $context.Stores.Root[$thumbprint] = $context.Stores.My[$thumbprint]
    Hit-Boundary 'ImportCertificateAfterMutation'
}
function Set-AuthenticodeSignature {
    [CmdletBinding()]
    param([string] $FilePath, $Certificate, $HashAlgorithm)
    Assert-FixturePath $FilePath
    Hit-Boundary 'SignExecutable'
    if ([IO.Path]::GetFileName($FilePath) -ceq 'GlideZoom-Uninstall.exe') { Hit-Boundary 'SignUninstaller' }
    [IO.File]::AppendAllText($FilePath, '\nsimulated-signature')
    (Get-TestContext).Signer = $Certificate
    if ((Get-TestContext).SignatureMode -ceq 'SignInvalid') {
        return [pscustomobject]@{ Status='HashMismatch'; StatusMessage='Injected invalid result'; SignerCertificate=$Certificate }
    }
    return [pscustomobject]@{ Status='Valid'; StatusMessage='Mock signature'; SignerCertificate=$Certificate }
}
function Get-AuthenticodeSignature {
    [CmdletBinding()]
    param([string] $LiteralPath)
    Assert-FixturePath $LiteralPath
    Hit-Boundary 'VerifySignature'
    if ((Get-TestContext).SignatureMode -ceq 'VerifyInvalid') {
        return [pscustomobject]@{ Status='UnknownError'; StatusMessage='Injected invalid result'; SignerCertificate=(Get-TestContext).Signer }
    }
    if ((Get-TestContext).SignatureMode -ceq 'WrongSigner') {
        return [pscustomobject]@{ Status='Valid'; StatusMessage='Injected wrong signer'; SignerCertificate=[pscustomobject]@{ Thumbprint=('B' * 40) } }
    }
    return [pscustomobject]@{ Status='Valid'; StatusMessage='Mock signature'; SignerCertificate=(Get-TestContext).Signer }
}
function Get-Process {
    [CmdletBinding()]
    param([string] $Name)
    Assert-Test ($Name -ceq 'GlideZoom') 'Unexpected process query'
    Hit-Boundary 'GetProcesses'
    $context = Get-TestContext
    switch ($context.ProcessMode) {
        'None' { return @() }
        'Owned' { return [pscustomobject]@{ Path=(Join-Path (Get-InstallPath) 'GlideZoom.exe') } }
        'Foreign' { return [pscustomobject]@{ Path=(Join-Path $context.Root 'other/GlideZoom.exe') } }
        'Unreadable' {
            $value = [pscustomobject]@{}
            $value | Add-Member ScriptProperty Path { throw 'Injected process path access denial' }
            return $value
        }
        default { throw 'Unexpected process fixture mode' }
    }
}
function Start-Process {
    [CmdletBinding()]
    param([string] $FilePath, [string] $ArgumentList, [string] $Verb, [switch] $PassThru)
    Assert-FixturePath $FilePath
    Assert-Test ($FilePath -ceq (Join-Path (Get-InstallPath) 'GlideZoom.exe') -and $ArgumentList -ceq '--quit' -and $Verb -ceq 'Open' -and $PassThru) 'Unexpected process launch'
    Hit-Boundary 'GracefulQuitLaunch'
    $context = Get-TestContext
    if ($context.QuitMode -ceq 'LaunchFailure') { throw 'Injected graceful quit launch failure' }
    $process = [pscustomobject]@{}
    $process | Add-Member ScriptMethod WaitForExit {
        param($Milliseconds)
        Assert-Test ($Milliseconds -eq 5000) 'Unexpected graceful quit timeout'
        Hit-Boundary 'GracefulQuitWait'
        $context = Get-TestContext
        if ($context.QuitMode -ceq 'Timeout') { return $false }
        if ($context.QuitMode -ceq 'Success') { $context.ProcessMode = 'None' }
        return $true
    }
    return $process
}
function Start-Sleep {
    [CmdletBinding()]
    param([int] $Milliseconds)
    Assert-Test ($Milliseconds -eq 250) 'Unexpected process wait interval'
    Hit-Boundary 'GracefulQuitPollDelay'
}
function Read-Host {
    param([string] $Prompt)
    Hit-Boundary 'Prompt'
    if ($Prompt -like 'Type YES*') { return (Get-TestContext).Answer }
    return ''
}
function Write-Host {
    param([Parameter(Position=0, ValueFromRemainingArguments=$true)] [object[]] $Object, $ForegroundColor)
    (Get-TestContext).Messages.Add(($Object -join ' '))
}

function New-Fixture([string] $Name) {
    # Failure boundary labels include registry separators such as ':'. Encode
    # only the fixture directory component for .NET Framework's path rules.
    $root = Join-Path $testRoot ([Uri]::EscapeDataString($Name))
    foreach ($path in @($root, (Join-Path $root 'source/scripts'), (Join-Path $root 'source/bin'),
                       (Join-Path $root 'ProgramFiles'), (Join-Path $root 'CommonPrograms'))) {
        [IO.Directory]::CreateDirectory($path) | Out-Null
    }
    $global:GlideZoomLifecycleTestContext = @{
        Root=$root; ProgramFiles=(Join-Path $root 'ProgramFiles'); CommonPrograms=(Join-Path $root 'CommonPrograms')
        Failure=''; Hits=@{}; Stores=@{ My=@{}; Root=@{}; TrustedPublisher=@{} }; Keys=@{}
        ExitCode=$null; Answer='YES'; IsAdministrator=$true; UnsafeAcl=$false; MutexAvailable=$true
        Signer=$null; SignatureMode='Valid'; Shortcut=$null; Registration=$null
        TokenOwner='S-1-5-32-544'; BadFileOwnerName=''; ProcessMode='None'; QuitMode='Success'
        GuiAnswer=$true; GuiOpen=$false; GuiResult=$null; BootstrapBusy=$false
        Messages=(Microsoft.PowerShell.Utility\New-Object 'System.Collections.Generic.List[string]')
    }
    Convert-TestScript (Join-Path $project 'scripts/Install.ps1') (Join-Path $root 'source/scripts/Install.ps1')
    Convert-TestScript (Join-Path $project 'scripts/Uninstall.ps1') (Join-Path $root 'source/scripts/Uninstall.ps1')
    [IO.File]::WriteAllText((Join-Path $root 'source/bin/GlideZoom.exe'), 'inert fixture; this is not an executable')
    [IO.File]::WriteAllText((Join-Path $root 'source/bin/GlideZoom-Uninstall.exe'), 'inert uninstaller fixture')
    [IO.File]::WriteAllText((Join-Path $root 'source/Uninstall.cmd'), 'inert fixture wrapper')
}
function Invoke-Install([switch] $Approved, [switch] $NonInteractive) {
    $context = Get-TestContext
    $context.ExitCode = $null
    & (Join-Path $context.Root 'source/scripts/Install.ps1') -ApprovedLocalTrust:$Approved -NonInteractive:$NonInteractive
    Assert-Test ($null -ne $context.ExitCode) ('Installer did not reach its exit; ' + ($context.Messages -join ' | '))
}
function Invoke-Uninstall([switch] $Gui, [uint32] $BootstrapProcessId = 0) {
    $context = Get-TestContext
    $context.ExitCode = $null
    & (Join-Path $context.Root 'source/scripts/Uninstall.ps1') -Gui:$Gui -BootstrapProcessId $BootstrapProcessId
    Assert-Test ($null -ne $context.ExitCode) ('Uninstaller did not reach its exit; ' + ($context.Messages -join ' | '))
}
function Get-InstallPath { return Join-Path (Get-TestContext).ProgramFiles 'GlideZoom' }
function Assert-Clean {
    $context = Get-TestContext
    Assert-Test (-not [IO.Directory]::Exists((Get-InstallPath))) 'Installation directory remains'
    Assert-Test ($context.Stores.My.Count -eq 0 -and $context.Stores.Root.Count -eq 0 -and $context.Stores.TrustedPublisher.Count -eq 0) 'Certificate trust remains'
    Assert-Test ($context.Keys.Count -eq 0) 'Signing key remains'
    Assert-Test ($null -eq $context.Registration) 'Installed Apps entry remains'
}
function Assert-Installed {
    $context = Get-TestContext
    Assert-Test ($context.ExitCode -eq 0) ('Install failed: ' + ($context.Messages -join ' | '))
    [string[]] $names = @([IO.Directory]::GetFiles((Get-InstallPath)) | ForEach-Object { [IO.Path]::GetFileName($_) })
    # Windows PowerShell and PowerShell 7 use different culture sort rules for
    # punctuation. Normalize order only; the exact footprint stays case-sensitive.
    [Array]::Sort($names, [StringComparer]::OrdinalIgnoreCase)
    Assert-Test (($names -join '|') -ceq 'GlideZoom-Uninstall.exe|GlideZoom.exe|install-manifest.json|Uninstall.cmd|Uninstall.ps1') ('Unexpected installed footprint: ' + ($names -join '|'))
    Assert-Test ($context.Stores.Root.Count -eq 1 -and $context.Stores.My.Count -eq 0 -and $context.Stores.TrustedPublisher.Count -eq 0) 'Unexpected installed trust stores'
    Assert-Test ($context.Keys.Count -eq 0) 'Private signing key retained on success'
    Assert-Test (-not $context.Hits.ContainsKey('Folder:CommonPrograms')) 'Schema3 touched CommonPrograms'
    Assert-Test ($null -ne $context.Registration -and $context.Registration.Values.Count -eq 10) 'Installed Apps entry missing or incomplete'
    Assert-Test ($context.Registration.Values.UninstallString -ceq ('"' + (Join-Path (Get-InstallPath) 'GlideZoom-Uninstall.exe') + '"')) 'Uninstaller command is not exactly quoted'
    Assert-Reached 'SignUninstaller'
}
function Complete-Case([string] $Name) {
    $results.Add($Name)
    $hits = [ordered]@{}
    foreach ($key in ((Get-TestContext).Hits.Keys | Sort-Object)) { $hits[$key] = (Get-TestContext).Hits[$key] }
    $caseRecords.Add([pscustomobject]@{ name=$Name; status='passed'; reached_mock_boundaries=$hits })
}
function Test-JournalReplacement([string] $ScriptName) {
    New-Fixture ('journal-' + $ScriptName)
    $source = Join-Path $project ('scripts/' + $ScriptName + '.ps1')
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref] $tokens, [ref] $errors)
    Assert-Test ($errors.Count -eq 0) 'Journal source did not parse'
    foreach ($name in @('Assert-RegularPath', 'Save-State')) {
        $definition = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name }, $true))
        Assert-Test ($definition.Count -eq 1) ('Expected one production function: ' + $name)
        . ([ScriptBlock]::Create($definition[0].Extent.Text))
    }
    $installDirectory = Get-InstallPath
    [IO.Directory]::CreateDirectory($installDirectory) | Out-Null
    $manifestPath = Join-Path $installDirectory 'install-manifest.json'
    [IO.File]::WriteAllText($manifestPath, '{"marker":"old","sequence":0}')
    $state = [ordered]@{ marker='new'; sequence=1 }
    Save-State
    $saved = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    Assert-Test ($saved.marker -ceq 'new' -and $saved.sequence -eq 1) 'Existing journal was not replaced'
    Assert-Test (-not [IO.File]::Exists($manifestPath + '.tmp')) 'Successful journal replacement left a temporary file'
    Complete-Case ($ScriptName + ' Save-State replaces an existing manifest using real file IO')
}

try {
    $env:PROCESSOR_ARCHITECTURE = 'AMD64'
    $env:PROCESSOR_ARCHITEW6432 = 'AMD64'

    New-Fixture 'test-path-type-parity'
    $fixtureLeaf = Join-Path (Get-TestContext).Root 'source/bin/GlideZoom.exe'
    $fixtureContainer = Join-Path (Get-TestContext).Root 'source/bin'
    Assert-Test (Test-Path -LiteralPath $fixtureLeaf -PathType Leaf) 'Mock must preserve native file PathType'
    Assert-Test (-not (Test-Path -LiteralPath $fixtureLeaf -PathType Container)) 'Mock must not classify a file as a container'
    Assert-Test (Test-Path -LiteralPath $fixtureContainer -PathType Container) 'Mock must preserve native directory PathType'
    Assert-Test (-not (Test-Path -LiteralPath $fixtureContainer -PathType Leaf)) 'Mock must not classify a directory as a leaf'
    Assert-Test (Test-Path -LiteralPath 'Cert:\LocalMachine\Root' -PathType Container) 'Fake certificate store is a container'
    Assert-Test (-not (Test-Path -LiteralPath 'Cert:\LocalMachine\Root' -PathType Leaf)) 'Fake certificate store is not a leaf'
    $fixtureThumbprint = 'A' * 40
    (Get-TestContext).Stores.Root[$fixtureThumbprint] = [pscustomobject]@{ Thumbprint=$fixtureThumbprint }
    $fixtureCertificate = 'Cert:\LocalMachine\Root\' + $fixtureThumbprint
    Assert-Test (Test-Path -LiteralPath $fixtureCertificate -PathType Leaf) 'Fake certificate entry is a leaf'
    Assert-Test (-not (Test-Path -LiteralPath $fixtureCertificate -PathType Container)) 'Fake certificate entry is not a container'
    Assert-Test (-not (Test-Path -LiteralPath ('Cert:\LocalMachine\Root\' + ('B' * 40)) -PathType Leaf)) 'Missing fake certificate remains absent'
    $outsideRejected = $false
    try { Test-Path -LiteralPath (Join-Path $testRoot 'outside-fixture') -PathType Container | Out-Null }
    catch { $outsideRejected = $_.Exception.Message.StartsWith('Test attempted filesystem access outside its fixture:') }
    Assert-Test $outsideRejected 'PathType must not bypass the fixture path guard'
    Complete-Case 'Test-Path file/container/certificate parity retains fixture-only access'

    Test-JournalReplacement 'Install'
    Test-JournalReplacement 'Uninstall'

    New-Fixture 'happy-unresolved-read-sid'
    Invoke-Install; Assert-Installed; Assert-Reached 'RawSidRules'; Assert-Reached 'DeleteKey'
    Invoke-Uninstall
    Assert-Test ((Get-TestContext).ExitCode -eq 0) 'Happy-path uninstall failed'
    Assert-Reached 'GetProcesses'; Assert-Clean
    Complete-Case 'raw unresolved read SID -> install -> exact uninstall'

    New-Fixture 'cancel'
    (Get-TestContext).Answer = 'NO'
    Invoke-Install; Assert-Reached 'Prompt'
    Assert-Test ((Get-TestContext).ExitCode -eq 2) 'Cancellation exit code'
    Assert-Clean; Complete-Case 'cancel before mutation'

    New-Fixture 'existing-directory'
    [IO.Directory]::CreateDirectory((Get-InstallPath)) | Out-Null
    $sentinel = Join-Path (Get-InstallPath) 'keep.txt'
    [IO.File]::WriteAllText($sentinel, 'outside data')
    Invoke-Install; Assert-Reached 'Folder:ProgramFiles'
    Assert-Test ((Get-TestContext).ExitCode -eq 1 -and [IO.File]::ReadAllText($sentinel) -ceq 'outside data') 'Existing data changed'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('CreateCertificate')) 'Existing folder reached certificate creation'
    Complete-Case 'existing directory and data preserved'

    New-Fixture 'unsafe-acl'
    (Get-TestContext).UnsafeAcl = $true
    Invoke-Install; Assert-Reached 'RawSidRules'
    Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Unknown writer was accepted'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('CreateCertificate')) 'Unsafe ACL reached certificate creation'
    Assert-Clean; Complete-Case 'unknown raw SID write permission rejected'

    New-Fixture 'unsupported-token-default-owner'
    (Get-TestContext).TokenOwner = 'S-1-5-21-111-222-333-1001'
    Invoke-Install -Approved -NonInteractive
    Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Nonprivileged token default owner was accepted'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('CreateCertificate')) 'Unsupported token owner reached signing'
    Assert-Test (-not [IO.Directory]::Exists((Get-InstallPath))) 'Unsupported token owner created installation directory'
    Assert-Clean; Complete-Case 'unsupported creating-token owner -> no files, key or trust mutation'

    New-Fixture 'unexpected-owned-file-owner'
    (Get-TestContext).BadFileOwnerName = 'GlideZoom-Uninstall.exe'
    Invoke-Install -Approved -NonInteractive; Assert-Reached 'FileOwnerRead'
    Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Unexpected created file owner was accepted'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('CreateCertificate')) 'Unexpected file owner reached trust mutation'
    Assert-Clean; Complete-Case 'unexpected copied uninstaller owner -> rollback before certificate creation'

    New-Fixture 'mutex-busy'
    (Get-TestContext).MutexAvailable = $false
    Invoke-Install; Assert-Reached 'MutexWait'
    Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Busy lifecycle mutex accepted'
    Assert-Clean; Complete-Case 'busy lifecycle mutex fails before mutation'

    foreach ($boundary in @('GetAcl', 'SetAcl', 'CopyExecutable', 'CopyUninstaller', 'CreateCertificate',
            'CreateCertificateAfterMutation', 'ExportCertificate', 'ImportCertificate',
            'ImportCertificateAfterMutation', 'SignExecutable', 'SignUninstaller', 'VerifySignature',
            'CreateRegistration', 'CreateRegistrationAfterMarker', 'WriteRegistration:DisplayName',
            'WriteRegistration:UninstallString', 'WriteRegistration:EstimatedSize', 'WriteRegistrationAfterMutation')) {
        New-Fixture ('failure-' + $boundary)
        (Get-TestContext).Failure = $boundary
        Invoke-Install; Assert-Reached $boundary
        Assert-Test ((Get-TestContext).ExitCode -eq 1) ('Injected failure was accepted: ' + $boundary)
        Assert-Clean
        (Get-TestContext).Failure = ''
        Invoke-Install; Assert-Installed
        Invoke-Uninstall; Assert-Clean
        Complete-Case ($boundary + ' -> rollback -> successful retry')
    }

    foreach ($mode in @('SignInvalid', 'VerifyInvalid', 'WrongSigner')) {
        New-Fixture ('signature-result-' + $mode)
        (Get-TestContext).SignatureMode = $mode
        Invoke-Install
        if ($mode -ceq 'SignInvalid') { Assert-Reached 'SignExecutable' } else { Assert-Reached 'VerifySignature' }
        Assert-Test ((Get-TestContext).ExitCode -eq 1) ('Signature result was accepted: ' + $mode)
        Assert-Clean
        Complete-Case ($mode + ' result -> rollback')
    }

    New-Fixture 'private-key-cleanup-failure'
    (Get-TestContext).Failure = 'DeleteKey'
    Invoke-Install; Assert-Reached 'DeleteKey'
    Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Private-key cleanup failure accepted'
    Assert-Test ([IO.File]::Exists((Join-Path (Get-InstallPath) 'install-manifest.json'))) 'Recovery manifest lost'
    Assert-Test ((Get-TestContext).Keys.Count -eq 1) 'Fixture did not retain failing key'
    (Get-TestContext).Failure = ''
    Invoke-Uninstall; Assert-Reached 'KeyOpen'; Assert-Clean
    Complete-Case 'private-key deletion failure retains journal; uninstall retry recovers'

    New-Fixture 'unknown-file'
    Invoke-Install; Assert-Installed
    $sentinel = Join-Path (Get-InstallPath) 'keep.txt'
    [IO.File]::WriteAllText($sentinel, 'outside data')
    Invoke-Uninstall
    Assert-Test ((Get-TestContext).ExitCode -eq 1 -and [IO.File]::ReadAllText($sentinel) -ceq 'outside data') 'Unknown file was not preserved'
    Assert-Test ((Get-TestContext).Stores.Root.Count -eq 1) 'Unknown file did not stop trust cleanup'
    [IO.File]::Delete($sentinel)
    Invoke-Uninstall; Assert-Clean
    Complete-Case 'unknown file blocks uninstall without deleting trust or data'

    foreach ($badManifest in @('{"SchemaVersion":2,', '{}')) {
        $name = if ($badManifest -ceq '{}') { 'missing-fields' } else { 'truncated-json' }
        New-Fixture ('manifest-' + $name)
        Invoke-Install; Assert-Installed
        $manifestPath = Join-Path (Get-InstallPath) 'install-manifest.json'
        $originalManifest = [IO.File]::ReadAllText($manifestPath)
        [IO.File]::WriteAllText($manifestPath, $badManifest)
        Invoke-Uninstall
        Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Corrupt manifest was accepted'
        Assert-Test ((Get-TestContext).Stores.Root.Count -eq 1) 'Corrupt manifest allowed trust deletion'
        Assert-Test (-not (Get-TestContext).Hits.ContainsKey('GetProcesses')) 'Corrupt manifest reached process shutdown'
        Assert-Test ([IO.File]::ReadAllText($manifestPath) -ceq $badManifest) 'Corrupt manifest was modified'
        [IO.File]::WriteAllText($manifestPath, $originalManifest)
        Invoke-Uninstall; Assert-Clean
        Complete-Case ($name + ' manifest -> no resource deletion -> corrected manifest retry')
    }

    foreach ($boundary in @('RemoveCertificate:Root', 'RemoveExecutable', 'RemoveRegistration', 'RemoveUninstaller')) {
        New-Fixture ('uninstall-failure-' + $boundary.Replace(':', '-'))
        Invoke-Install; Assert-Installed
        (Get-TestContext).Failure = $boundary
        Invoke-Uninstall; Assert-Reached $boundary
        Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Uninstall failure reported success'
        Assert-Test ([IO.File]::Exists((Join-Path (Get-InstallPath) 'install-manifest.json'))) 'Uninstall failure lost manifest'
        (Get-TestContext).Failure = ''
        Invoke-Uninstall; Assert-Clean
        Complete-Case ($boundary + ' -> retained journal -> uninstall retry')
    }

    New-Fixture 'legacy-schema-one'
    Invoke-Install; Assert-Installed
    $context = Get-TestContext
    $manifestPath = Join-Path (Get-InstallPath) 'install-manifest.json'
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $manifest.SchemaVersion = 1
    $context.Registration = $null
    [IO.File]::Delete((Join-Path (Get-InstallPath) 'GlideZoom-Uninstall.exe'))
    $shortcutPath = Join-Path $context.CommonPrograms 'GlideZoom.lnk'
    $manifest | Add-Member NoteProperty StartMenuShortcut $shortcutPath
    $manifest.OwnedCertificateStores = @('My', 'Root', 'TrustedPublisher')
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5))
    [IO.File]::WriteAllText($shortcutPath, 'inert shortcut fixture')
    $context.Shortcut = [pscustomobject]@{ TargetPath=(Join-Path (Get-InstallPath) 'GlideZoom.exe'); Arguments='' }
    $context.Stores.TrustedPublisher[$manifest.CertificateThumbprint] = $context.Stores.Root[$manifest.CertificateThumbprint]
    $context.Shortcut.TargetPath = Join-Path $context.Root 'unrelated.exe'
    Invoke-Uninstall; Assert-Reached 'LegacyShortcutOpen'
    Assert-Test ($context.ExitCode -eq 1) 'Changed legacy shortcut was accepted'
    Assert-Test ($context.Stores.Root.Count -eq 1 -and $context.Stores.TrustedPublisher.Count -eq 1) 'Changed shortcut allowed trust deletion'
    Assert-Test ([IO.File]::Exists($shortcutPath)) 'Changed legacy shortcut was deleted'
    Complete-Case 'changed schema1 shortcut blocks uninstall without deletion'
    $context.Shortcut.TargetPath = Join-Path (Get-InstallPath) 'GlideZoom.exe'
    Invoke-Uninstall; Assert-Reached 'LegacyShortcutOpen'; Assert-Reached 'RemoveCertificate:TrustedPublisher'
    Assert-Clean; Assert-Test (-not [IO.File]::Exists($shortcutPath)) 'Legacy shortcut remains'
    Complete-Case 'schema1 exact shortcut and TrustedPublisher cleanup'

    New-Fixture 'gui-happy-install-uninstall'
    Invoke-Install -Approved -NonInteractive; Assert-Installed
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('Prompt')) 'Noninteractive install asked for console input'
    Invoke-Uninstall -Gui -BootstrapProcessId 901
    Assert-Reached 'GuiConfirm'; Assert-Reached 'BootstrapWait'; Assert-Reached 'GuiComplete'
    Assert-Test ((Get-TestContext).GuiResult -eq 0 -and -not (Get-TestContext).GuiOpen) 'GUI did not complete successfully'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('Prompt')) 'GUI uninstall asked for console input'
    Assert-Clean; Complete-Case 'approved GUI install -> GUI uninstall, no console prompts or delayed deletion'

    foreach ($combination in @(@($true, $false), @($false, $true))) {
        New-Fixture ('invalid-consent-flags-' + $combination[0])
        Invoke-Install -Approved:$combination[0] -NonInteractive:$combination[1]
        Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Incomplete explicit-approval contract was accepted'
        Assert-Test (-not (Get-TestContext).Hits.ContainsKey('CreateCertificate')) 'Invalid approval reached mutation'
        Assert-Clean; Complete-Case ('mismatched approved/noninteractive flags ' + ($combination -join '/') + ' -> no mutation')
    }

    New-Fixture 'gui-cancel-removal'
    Invoke-Install -Approved -NonInteractive; Assert-Installed
    (Get-TestContext).GuiAnswer = $false
    Invoke-Uninstall -Gui
    Assert-Test ((Get-TestContext).ExitCode -eq 2 -and (Get-TestContext).Stores.Root.Count -eq 1) 'GUI cancellation changed trust'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('RemoveRegistration')) 'GUI cancellation changed Installed Apps'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('GuiProgressStart')) 'GUI cancellation started work'
    (Get-TestContext).GuiAnswer = $true
    Invoke-Uninstall -Gui; Assert-Clean
    Complete-Case 'GUI removal cancellation preserves trust and files, subsequent GUI removal succeeds'

    New-Fixture 'bootstrap-still-running'
    Invoke-Install -Approved -NonInteractive; Assert-Installed
    (Get-TestContext).BootstrapBusy = $true
    Invoke-Uninstall -Gui -BootstrapProcessId 901
    Assert-Reached 'BootstrapWait'
    Assert-Test ((Get-TestContext).ExitCode -eq 1 -and (Get-TestContext).Stores.Root.Count -eq 1) 'Live native launcher did not block deletion'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('GuiConfirm')) 'Unverified launcher reached consent'
    (Get-TestContext).BootstrapBusy = $false
    Invoke-Uninstall -Gui -BootstrapProcessId 901; Assert-Clean
    Complete-Case 'live launcher -> no deletion -> normal launcher exit -> GUI retry'

    foreach ($boundary in @('GuiInitialize', 'GuiConfirm', 'GuiProgressStart', 'GuiProgressUpdate')) {
        New-Fixture ('gui-failure-' + $boundary)
        Invoke-Install -Approved -NonInteractive; Assert-Installed
        (Get-TestContext).Failure = $boundary
        Invoke-Uninstall -Gui
        Assert-Reached $boundary
        Assert-Test ((Get-TestContext).ExitCode -eq 1 -and (Get-TestContext).Stores.Root.Count -eq 1) 'GUI startup failure allowed trust deletion'
        Assert-Test ($null -ne (Get-TestContext).Registration) 'GUI failure removed Installed Apps entry'
        if ($boundary -ceq 'GuiInitialize') { Assert-Reached 'GuiFallback' }
        (Get-TestContext).Failure = ''
        Invoke-Uninstall -Gui; Assert-Clean
        Complete-Case ($boundary + ' failure -> visible failure path -> no trust loss -> GUI retry')
    }

    New-Fixture 'existing-foreign-registration'
    $foreign = [pscustomobject]@{ Values=@{ InstallationId=('f' * 32); DisplayName='Other app' }; Kinds=@{InstallationId='String'; DisplayName='String'}; SubKeys=@() }
    (Get-TestContext).Registration = $foreign
    Invoke-Install -Approved -NonInteractive
    Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Existing Installed Apps entry was accepted'
    Assert-Test ((Get-TestContext).Registration.Values.DisplayName -ceq 'Other app') 'Foreign Installed Apps data changed'
    Assert-Test (-not [IO.Directory]::Exists((Get-InstallPath))) 'Foreign registration reached directory creation'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('CreateCertificate')) 'Foreign registration reached certificate mutation'
    Complete-Case 'preexisting unrelated Installed Apps entry remains byte-for-byte unchanged'

    foreach ($change in @('Owner', 'Command', 'UnknownValue', 'Subkey', 'ValueKind')) {
        foreach ($guiMode in @($false, $true)) {
            New-Fixture ('registration-change-' + $change + '-' + $guiMode)
            Invoke-Install -Approved -NonInteractive; Assert-Installed
            $entry = (Get-TestContext).Registration
            $original = [pscustomobject]@{ Values=$entry.Values.Clone(); Kinds=$entry.Kinds.Clone(); SubKeys=@($entry.SubKeys) }
            switch ($change) {
                'Owner' { $entry.Values.InstallationId = 'f' * 32 }
                'Command' { $entry.Values.UninstallString = '"C:\unrelated.exe"' }
                'UnknownValue' { $entry.Values.Foreign = 'keep'; $entry.Kinds.Foreign = 'String' }
                'Subkey' { $entry.SubKeys = @('unrelated') }
                'ValueKind' { $entry.Kinds.DisplayName = 'ExpandString' }
            }
            $snapshot = $entry | ConvertTo-Json -Depth 5 -Compress
            Invoke-Uninstall -Gui:$guiMode
            Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Changed registration was accepted'
            Assert-Test (((Get-TestContext).Registration | ConvertTo-Json -Depth 5 -Compress) -ceq $snapshot) 'Changed registration was modified'
            Assert-Test ((Get-TestContext).Stores.Root.Count -eq 1) 'Changed registration allowed certificate removal'
            Assert-Test (-not (Get-TestContext).Hits.ContainsKey('GetProcesses')) 'Changed registration reached process shutdown'
            (Get-TestContext).Registration = $original
            Invoke-Uninstall -Gui:$guiMode; Assert-Clean
            Complete-Case ($change + ' Installed Apps interference + GUI=' + $guiMode + ' -> preserve -> corrected identity retry')
        }
    }

    foreach ($file in @('Uninstall.ps1', 'GlideZoom-Uninstall.exe')) {
        New-Fixture ('changed-uninstaller-' + $file)
        Invoke-Install -Approved -NonInteractive; Assert-Installed
        $path = Join-Path (Get-InstallPath) $file
        $original = [IO.File]::ReadAllBytes($path)
        [IO.File]::AppendAllText($path, 'external modification')
        Invoke-Uninstall -Gui
        Assert-Test ((Get-TestContext).ExitCode -eq 1 -and (Get-TestContext).Stores.Root.Count -eq 1) 'Changed uninstaller did not preserve trust'
        Assert-Test (-not (Get-TestContext).Hits.ContainsKey('RemoveRegistration')) 'Changed uninstaller removed Installed Apps entry'
        [IO.File]::WriteAllBytes($path, $original)
        Invoke-Uninstall -Gui; Assert-Clean
        Complete-Case ($file + ' hash mismatch -> preserve -> original-file retry')
    }

    foreach ($quitMode in @('Success', 'LaunchFailure', 'Timeout', 'StillRunning')) {
        New-Fixture ('running-app-' + $quitMode)
        Invoke-Install -Approved -NonInteractive; Assert-Installed
        $context = Get-TestContext
        $context.ProcessMode = 'Owned'; $context.QuitMode = $quitMode
        Invoke-Uninstall -Gui
        Assert-Reached 'GracefulQuitLaunch'
        if ($quitMode -ceq 'Success') {
            Assert-Test ($context.ExitCode -eq 0) 'Graceful app exit did not allow uninstall'
            Assert-Clean
        } else {
            Assert-Test ($context.ExitCode -eq 1 -and $context.Stores.Root.Count -eq 1) 'Failed graceful quit allowed trust deletion'
            Assert-Test ($null -ne $context.Registration) 'Failed graceful quit removed Installed Apps entry'
            Assert-Test ([IO.File]::Exists((Join-Path (Get-InstallPath) 'GlideZoom.exe'))) 'Failed graceful quit removed executable'
            if ($quitMode -ceq 'StillRunning') {
                Assert-Test ($context.Hits.GracefulQuitPollDelay -eq 20) 'Unresponsive app did not use the bounded polling budget'
            }
            $context.QuitMode = 'Success'
            Invoke-Uninstall -Gui; Assert-Clean
        }
        Complete-Case ('running owned app + ' + $quitMode + ' -> bounded graceful quit, no force-kill, retryable failure')
    }

    New-Fixture 'running-foreign-same-name-process'
    Invoke-Install -Approved -NonInteractive; Assert-Installed
    (Get-TestContext).ProcessMode = 'Foreign'
    Invoke-Uninstall -Gui; Assert-Clean
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('GracefulQuitLaunch')) 'Unrelated same-name process was targeted'
    Assert-Test ((Get-TestContext).ProcessMode -ceq 'Foreign') 'Unrelated process state changed'
    Complete-Case 'unrelated same-name process is neither quit nor terminated'

    New-Fixture 'uninspectable-process'
    Invoke-Install -Approved -NonInteractive; Assert-Installed
    (Get-TestContext).ProcessMode = 'Unreadable'
    Invoke-Uninstall -Gui
    Assert-Test ((Get-TestContext).ExitCode -eq 1 -and (Get-TestContext).Stores.Root.Count -eq 1) 'Uninspectable process allowed removal'
    Assert-Test (-not (Get-TestContext).Hits.ContainsKey('GracefulQuitLaunch')) 'Uninspectable process was targeted'
    (Get-TestContext).ProcessMode = 'None'
    Invoke-Uninstall -Gui; Assert-Clean
    Complete-Case 'process-path inspection failure -> retain trust/files -> manual exit and retry'

    New-Fixture 'changed-running-executable'
    Invoke-Install -Approved -NonInteractive; Assert-Installed
    $context = Get-TestContext; $context.ProcessMode = 'Owned'
    $path = Join-Path (Get-InstallPath) 'GlideZoom.exe'
    $original = [IO.File]::ReadAllBytes($path)
    [IO.File]::AppendAllText($path, 'external change')
    Invoke-Uninstall -Gui
    Assert-Test ($context.ExitCode -eq 1 -and $context.Stores.Root.Count -eq 1) 'Changed running executable allowed removal'
    Assert-Test (-not $context.Hits.ContainsKey('GracefulQuitLaunch')) 'Changed executable was launched elevated for quit'
    [IO.File]::WriteAllBytes($path, $original)
    Invoke-Uninstall -Gui; Assert-Clean
    Complete-Case 'running app hash mismatch -> no quit launch or trust deletion -> original-file retry'

    New-Fixture 'registry-create-before-owner-marker'
    (Get-TestContext).Failure = 'CreateRegistrationBeforeMarker'
    Invoke-Install -Approved -NonInteractive; Assert-Reached 'CreateRegistrationBeforeMarker'
    Assert-Test ((Get-TestContext).ExitCode -eq 1) 'Unidentified registry creation reported success'
    Assert-Test ([IO.File]::Exists((Join-Path (Get-InstallPath) 'install-manifest.json'))) 'Ambiguous ownership lost recovery journal'
    Assert-Test ((Get-TestContext).Registration.Values.Count -eq 0) 'Unowned registry key was changed or guessed'
    Assert-Test ((Get-TestContext).Stores.Root.Count -eq 0 -and (Get-TestContext).Keys.Count -eq 0) 'Independent trust rollback did not complete'
    (Get-TestContext).Failure = ''
    Invoke-Uninstall -Gui
    Assert-Test ((Get-TestContext).ExitCode -eq 1 -and (Get-TestContext).Registration.Values.Count -eq 0) 'Recovery guessed ownership of an empty entry'
    # Model an administrator explicitly reviewing/removing that one ambiguous empty key.
    (Get-TestContext).Registration = $null
    Invoke-Uninstall -Gui; Assert-Clean
    Complete-Case 'failure before registry owner marker -> retain exact journal and ambiguous key; no ownership guessing'

    New-Fixture 'legacy-schema-two'
    Invoke-Install -Approved -NonInteractive; Assert-Installed
    $context = Get-TestContext
    $manifestPath = Join-Path (Get-InstallPath) 'install-manifest.json'
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $manifest.SchemaVersion = 2
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5))
    [IO.File]::Delete((Join-Path (Get-InstallPath) 'GlideZoom-Uninstall.exe'))
    $context.Registration = $null
    Invoke-Uninstall -Gui; Assert-Clean
    Complete-Case 'legacy schema2 removal works through the current GUI script and preserves preferences'

    foreach ($relative in $runHashes.Keys) {
        $sourceStream = [IO.File]::OpenRead((Join-Path $project $relative))
        try { $currentHash = (Microsoft.PowerShell.Utility\Get-FileHash -InputStream $sourceStream -Algorithm SHA256).Hash.ToLowerInvariant() }
        finally { $sourceStream.Dispose() }
        Assert-Test ($currentHash -ceq $runHashes[$relative]) ('Source changed during test run: ' + $relative)
    }
    [Console]::WriteLine(('PASS: {0} installer lifecycle scenarios; Windows APIs mocked, fixture file/journal IO real.' -f $results.Count))
    foreach ($result in $results) { [Console]::WriteLine('  ' + $result) }
    if ($ReportPath) {
        $os = if ($PSVersionTable.ContainsKey('OS')) { $PSVersionTable.OS } else { [Environment]::OSVersion.VersionString }
        $report = [ordered]@{
            schema_version=1; generated_utc=[DateTime]::UtcNow.ToString('o')
            result='passed'; scenarios=$results.Count; failures=0
            runtime=[ordered]@{ powershell=$PSVersionTable.PSVersion.ToString(); os=$os }
            native_windows_execution=$false
            scope='Production PowerShell control flow, validators, rollback, and JSON journal IO; Windows service boundaries simulated.'
            real_operations=@('FileStream creation/write/flush', 'File.Move', 'File.Replace', 'file hashing', 'file copy/delete', 'JSON serialization and parsing')
            mocked_boundaries=@('WindowsIdentity and administrator token', 'special folders', 'lifecycle mutex', 'ACL read/write and SID rule objects', 'certificate stores and certificate generation/import/export', 'Authenticode signing and verification', 'CNG key existence/open/deletion', 'process enumeration and bounded graceful quit launch/wait/polling', 'legacy shortcut COM', 'interactive prompts', '64-bit HKLM registry read/create/write/delete', 'WinForms dialogs and progress', 'native uninstaller launcher identity/wait')
            ast_replacements=@('WindowsIdentity.GetCurrent', 'Environment.GetFolderPath', 'Threading.Mutex.new', 'CngKey.Exists/Open', 'exact native registry/GUI/bootstrap functions replaced at their boundary', 'exit records the code and returns')
            limitations=@('Runtime version/OS above are the only executed host, not a claim of other hosts', 'No actual ACL/certificate/CNG/Authenticode provider calls, including when run on Windows', 'No real process shutdown or simultaneous installer processes; quit results and polling are simulated', 'No guarantee against arbitrary OS or hardware failures')
            source_sha256=$runHashes; cases=@($caseRecords.ToArray())
        }
        [IO.File]::WriteAllText([IO.Path]::GetFullPath($ReportPath), ($report | ConvertTo-Json -Depth 12))
        [Console]::WriteLine('Report: ' + [IO.Path]::GetFullPath($ReportPath))
    }
} finally {
    $env:PROCESSOR_ARCHITECTURE = $oldArchitecture
    $env:PROCESSOR_ARCHITEW6432 = $oldWowArchitecture
    Remove-Variable -Name $contextName -Scope Global -ErrorAction SilentlyContinue
    if ([IO.Directory]::Exists($testRoot)) { [IO.Directory]::Delete($testRoot, $true) }
}
