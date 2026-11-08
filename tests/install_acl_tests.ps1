# Run with Windows PowerShell 5.1 or PowerShell 7. No Pester or elevation needed.
# This executes the production policy with fake ACLs. Windows also gets in-memory
# DirectorySecurity fixtures; no real directory ACL, file, or trust store is changed.
[CmdletBinding()]
param([string] $JsonPath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:assertions = 0
$script:cases = 0
$script:failures = New-Object 'System.Collections.Generic.List[object]'
$installer = Join-Path $PSScriptRoot '..\scripts\Install.ps1'
$sourceHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
$testHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    [IO.Path]::GetFullPath($installer), [ref] $tokens, [ref] $parseErrors)
if ($parseErrors.Count -ne 0) { throw 'The production installer has PowerShell parse errors.' }
$policy = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Assert-TrustedDirectoryAcl'
}, $true))
if ($policy.Count -ne 1) { throw 'Expected exactly one production ACL policy function.' }
# Load only this function, never the installer's top-level code.
. ([scriptblock]::Create($policy[0].Extent.Text))

function Assert-Check([bool] $Condition, [string] $Message) {
    $script:assertions++
    if (-not $Condition) { throw $Message }
}

function New-FakeRule {
    param(
        [string] $Sid,
        [Security.AccessControl.FileSystemRights] $Rights,
        [Security.AccessControl.AccessControlType] $Kind = 'Allow',
        [Security.AccessControl.PropagationFlags] $Propagation = 'None',
        [bool] $Inherited = $false
    )
    $identity = [pscustomobject]@{ Value = $Sid; TranslateCalls = 0 }
    $identity | Add-Member ScriptMethod Translate {
        param($targetType)
        $this.TranslateCalls++
        throw 'Some or all identity references could not be translated.'
    }
    [pscustomobject]@{
        IdentityReference = $identity
        FileSystemRights = $Rights
        AccessControlType = $Kind
        InheritanceFlags = [Security.AccessControl.InheritanceFlags] 'ContainerInherit, ObjectInherit'
        PropagationFlags = $Propagation
        IsInherited = $Inherited
    }
}

function New-FakeAcl {
    param([object[]] $Rules, [bool] $Protected = $false, [bool] $ReadFails = $false)
    $acl = [pscustomobject]@{
        Rules = $Rules
        AreAccessRulesProtected = $Protected
        ReadFails = $ReadFails
        AccessReads = 0
        Calls = New-Object 'System.Collections.Generic.List[object]'
    }
    $acl | Add-Member ScriptProperty Access {
        $this.AccessReads++
        throw 'Policy must not request account-name ACLs through .Access.'
    }
    $acl | Add-Member ScriptMethod GetAccessRules {
        param([bool] $includeExplicit, [bool] $includeInherited, [type] $targetType)
        $this.Calls.Add([pscustomobject]@{
            Explicit = $includeExplicit; Inherited = $includeInherited; Target = $targetType
        })
        if ($this.ReadFails) { throw 'Injected ACL enumeration failure.' }
        return ,$this.Rules
    }
    return $acl
}

function Test-Policy {
    param(
        [string] $Name,
        [object[]] $Rules,
        [bool] $Reject,
        [bool] $Protected = $false,
        [bool] $ReadFails = $false,
        [string] $ErrorContains = ''
    )
    $script:cases++
    try {
        $acl = New-FakeAcl $Rules $Protected $ReadFails
        $failure = $null
        try { Assert-TrustedDirectoryAcl $acl } catch { $failure = $_.Exception.Message }
        Assert-Check (($null -ne $failure) -eq $Reject) "Unexpected policy result: $failure"
        if ($Reject -and $ErrorContains) {
            Assert-Check ($failure.Contains($ErrorContains)) "Wrong rejection: $failure"
        }
        $expectedReads = if ($Protected) { 0 } else { 1 }
        Assert-Check ($acl.Calls.Count -eq $expectedReads) 'Wrong number of SID ACL enumerations.'
        foreach ($call in $acl.Calls) {
            Assert-Check ($call.Explicit -and $call.Inherited) 'Explicit or inherited ACEs were omitted.'
            Assert-Check ($call.Target -eq [Security.Principal.SecurityIdentifier]) 'Account names were requested.'
        }
        Assert-Check ($acl.AccessReads -eq 0) 'The policy used .Access.'
        foreach ($rule in $Rules) {
            Assert-Check ($rule.IdentityReference.TranslateCalls -eq 0) 'The policy translated an account name.'
        }
    } catch {
        $script:failures.Add([pscustomobject]@{ name = $Name; error = $_.Exception.Message })
    }
}

$unknownSid = 'S-1-5-21-111111111-222222222-333333333-9876'
$script:cases++
$legacyReproduced = $false
try {
    $legacyRule = New-FakeRule $unknownSid 'ReadAndExecute'
    $legacyAcl = [pscustomobject]@{ Access = @($legacyRule) }
    $legacyFailure = $null
    try {
        # Negative control: this was the failing lookup in the shipped installer.
        foreach ($rule in $legacyAcl.Access) {
            $null = $rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
        }
    } catch { $legacyFailure = $_.Exception.Message }
    Assert-Check ($null -ne $legacyFailure) 'The old name-translation algorithm unexpectedly passed.'
    Assert-Check ($legacyFailure.Contains('identity references could not be translated')) 'Wrong negative-control failure.'
    Assert-Check ($legacyRule.IdentityReference.TranslateCalls -eq 1) 'The old algorithm did not exercise the failing lookup.'
    $legacyReproduced = $true
} catch {
    $script:failures.Add([pscustomobject]@{ name = 'legacy-unresolved-reader-negative-control'; error = $_.Exception.Message })
}

Test-Policy 'unresolved-reader' @((New-FakeRule $unknownSid 'ReadAndExecute')) $false
Test-Policy 'unresolved-deny' @((New-FakeRule $unknownSid 'FullControl' 'Deny')) $false
Test-Policy 'unresolved-writer' @((New-FakeRule $unknownSid 'Write')) $true -ErrorContains $unknownSid
Test-Policy 'enumeration-failure' @() $true -ReadFails $true -ErrorContains 'Injected ACL enumeration failure'
Test-Policy 'protected-acl' @() $true -Protected $true
Test-Policy 'empty-acl' @() $true

$sidCases = @(
    @{ name = 'system'; sid = 'S-1-5-18'; privileged = $true },
    @{ name = 'administrators'; sid = 'S-1-5-32-544'; privileged = $true },
    @{ name = 'trusted-installer'; sid = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'; privileged = $true },
    @{ name = 'creator-owner'; sid = 'S-1-3-0'; privileged = $true },
    @{ name = 'everyone'; sid = 'S-1-1-0'; privileged = $false },
    @{ name = 'users'; sid = 'S-1-5-32-545'; privileged = $false },
    @{ name = 'authenticated-users'; sid = 'S-1-5-11'; privileged = $false },
    @{ name = 'unresolved-local'; sid = $unknownSid; privileged = $false },
    @{ name = 'unresolved-aad'; sid = 'S-1-12-1-111111-222222-333333-444444'; privileged = $false },
    @{ name = 'unresolved-service'; sid = 'S-1-5-80-11111-22222-33333-44444-55555'; privileged = $false }
)
$rightsCases = @(
    @{ name = 'ReadAndExecute'; writes = $false },
    @{ name = 'ReadPermissions'; writes = $false },
    @{ name = 'Synchronize'; writes = $false },
    @{ name = 'Write'; writes = $true },
    @{ name = 'WriteData'; writes = $true },
    @{ name = 'AppendData'; writes = $true },
    @{ name = 'WriteExtendedAttributes'; writes = $true },
    @{ name = 'WriteAttributes'; writes = $true },
    @{ name = 'Delete'; writes = $true },
    @{ name = 'DeleteSubdirectoriesAndFiles'; writes = $true },
    @{ name = 'ChangePermissions'; writes = $true },
    @{ name = 'TakeOwnership'; writes = $true },
    @{ name = 'FullControl'; writes = $true },
    @{ name = [Enum]::ToObject([Security.AccessControl.FileSystemRights], 0x10000000); writes = $true },
    @{ name = [Enum]::ToObject([Security.AccessControl.FileSystemRights], 0x40000000); writes = $true }
)
$propagationCases = @('None', 'InheritOnly', 'NoPropagateInherit', 'InheritOnly, NoPropagateInherit')
$matrixStart = $script:cases
foreach ($sidCase in $sidCases) {
    foreach ($rightsCase in $rightsCases) {
        foreach ($kind in @('Allow', 'Deny')) {
            foreach ($propagation in $propagationCases) {
                foreach ($inherited in @($false, $true)) {
                    $reject = $kind -eq 'Allow' -and $rightsCase.writes -and -not $sidCase.privileged
                    $rule = New-FakeRule $sidCase.sid $rightsCase.name $kind $propagation $inherited
                    Test-Policy ($sidCase.name + '/' + $rightsCase.name + '/' + $kind + '/' + $propagation + '/' + $inherited) @($rule) $reject
                }
            }
        }
    }
}
$matrixCases = $script:cases - $matrixStart

# Mixed rules check ordering and that a harmless/deny/privileged ACE cannot hide
# one unsafe inherited ACE. Deny rules are not used to waive a broad Allow rule.
$mixedStart = $script:cases
foreach ($position in 0..7) {
    foreach ($unsafe in @($false, $true)) {
        foreach ($reverse in @($false, $true)) {
            $rules = @(
                (New-FakeRule 'S-1-5-18' 'FullControl'),
                (New-FakeRule 'S-1-5-32-544' 'FullControl'),
                (New-FakeRule 'S-1-3-0' 'FullControl' 'Allow' 'InheritOnly'),
                (New-FakeRule $unknownSid 'ReadAndExecute'),
                (New-FakeRule 'S-1-1-0' 'FullControl' 'Deny'),
                (New-FakeRule 'S-1-5-32-545' 'ReadAndExecute'),
                (New-FakeRule 'S-1-5-11' 'ReadPermissions')
            )
            $candidateRights = if ($unsafe) { 'ChangePermissions' } else { 'ReadAndExecute' }
            $list = New-Object 'System.Collections.Generic.List[object]'
            foreach ($rule in $rules) { $list.Add($rule) }
            $list.Insert($position, (New-FakeRule $unknownSid $candidateRights 'Allow' 'InheritOnly' $true))
            if ($reverse) { $list.Reverse() }
            Test-Policy "mixed/$position/$unsafe/$reverse" $list.ToArray() $unsafe
        }
    }
}
$mixedCases = $script:cases - $mixedStart

$nativeCount = 0
$nativeSkipped = 9
if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    $nativeFixtures = @(
        @{ name = 'native-unresolved-reader'; sddl = "O:BAG:BAD:AI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FR;;;$unknownSid)"; reject = $false },
        @{ name = 'native-unresolved-deny'; sddl = "O:BAG:BAD:AI(D;OICI;FA;;;$unknownSid)(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)"; reject = $false },
        @{ name = 'native-generic-all-writer'; sddl = "O:BAG:BAD:AI(A;OICI;FA;;;BA)(A;OICI;GA;;;$unknownSid)"; reject = $true },
        @{ name = 'native-generic-write-inherit-only'; sddl = "O:BAG:BAD:AI(A;OICI;FA;;;BA)(A;OICIIO;GW;;;$unknownSid)"; reject = $true },
        @{ name = 'native-unresolved-writer'; sddl = "O:BAG:BAD:AI(A;OICI;FA;;;BA)(A;OICI;FW;;;$unknownSid)"; reject = $true },
        @{ name = 'native-inherit-only-writer'; sddl = "O:BAG:BAD:AI(A;OICI;FA;;;BA)(A;OICIIO;FW;;;$unknownSid)"; reject = $true },
        @{ name = 'native-protected-acl'; sddl = 'O:BAG:BAD:P(A;OICI;FA;;;BA)'; reject = $true },
        @{ name = 'native-empty-dacl'; sddl = 'O:BAG:BAD:AI'; reject = $true },
        @{ name = 'native-null-dacl'; sddl = 'O:BAG:BA'; reject = $true }
    )
    foreach ($fixture in $nativeFixtures) {
        $script:cases++
        $nativeCount++
        try {
            $acl = New-Object Security.AccessControl.DirectorySecurity
            $acl.SetSecurityDescriptorSddlForm($fixture.sddl)
            $failure = $null
            try { Assert-TrustedDirectoryAcl $acl } catch { $failure = $_.Exception.Message }
            Assert-Check (($null -ne $failure) -eq $fixture.reject) "Unexpected native-object policy result: $failure"
        } catch {
            $script:failures.Add([pscustomobject]@{ name = $fixture.name; error = $_.Exception.Message })
        }
    }
    $nativeSkipped = 0
}

$script:cases++
try {
    Assert-Check ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant() -eq $sourceHash) 'Installer source changed during the run.'
    Assert-Check ((Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() -eq $testHash) 'Test source changed during the run.'
} catch {
    $script:failures.Add([pscustomobject]@{ name = 'test-inputs-unchanged'; error = $_.Exception.Message })
}

$summary = [ordered]@{
    suite = 'install_acl'
    powershell_version = $PSVersionTable.PSVersion.ToString()
    platform = [Environment]::OSVersion.Platform.ToString()
    source_sha256 = $sourceHash
    test_sha256 = $testHash
    scope = 'Production ACL policy executed with fake ACLs; real in-memory DirectorySecurity objects run on Windows only. No actual installation or machine ACL modification.'
    cases = $script:cases
    assertions = $script:assertions
    matrix_cases = $matrixCases
    mixed_rule_cases = $mixedCases
    legacy_failure_reproduced_with_mock = $legacyReproduced
    native_in_memory_cases = $nativeCount
    native_in_memory_skipped = $nativeSkipped
    native_installation_tested = $false
    failures = @($script:failures.ToArray())
    passed = $script:failures.Count -eq 0
}
$json = $summary | ConvertTo-Json -Depth 6
if ($JsonPath) { [IO.File]::WriteAllText([IO.Path]::GetFullPath($JsonPath), $json + [Environment]::NewLine) }
Write-Output $json
if ($script:failures.Count -ne 0) { exit 1 }
exit 0
