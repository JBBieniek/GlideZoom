# Executes the command prefix/suffix extracted from production setup_policy.h.
# Only isolated fixture scripts are invoked. Install.ps1 is NEVER executed.
# Run on Windows PowerShell 5.1 or PowerShell 7, without elevation or Pester.
[CmdletBinding()]
param([string] $ReportPath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $project 'src/setup_policy.h'
$source = [IO.File]::ReadAllText($sourcePath)
$sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
$testHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('GlideZoomSetupPowerShell-' + [Guid]::NewGuid().ToString('N'))
$powerShellHost = (Get-Process -Id $PID).Path
$records = New-Object 'System.Collections.Generic.List[object]'
$script:assertions = 0

function Assert-Test([bool] $Condition, [string] $Message) {
    $script:assertions++
    if (-not $Condition) { throw ('ASSERTION: ' + $Message) }
}
function Decode-CppLiteral([string] $Value) {
    return [regex]::Replace($Value, '\\(.)', {
        param($match)
        switch -CaseSensitive ($match.Groups[1].Value) {
            '\' { return '\' }
            '"' { return '"' }
            'n' { return "`n" }
            'r' { return "`r" }
            't' { return "`t" }
            default { throw ('Unsupported C++ escape in production command: ' + $match.Value) }
        }
    })
}
# Deliberately fail if the production expression changes shape, instead of
# silently testing an obsolete duplicate. C++ setup_tests independently execute
# CommandLine/QuoteArgument/PowerShellLiteral themselves and their argument oracle.
$pattern = '(?s)inline\s+std::wstring\s+CommandLine\(std::wstring_view executable,\s*std::wstring_view script\)\s*\{\s*const std::wstring command\s*=\s*L"(?<prefix>(?:\\.|[^"\\])*)"\s*\+\s*PowerShellLiteral\(script\)\s*\+\s*L"(?<suffix>(?:\\.|[^"\\])*)";\s*return QuoteArgument\(executable\)\s*\+\s*L"(?<args>(?:\\.|[^"\\])*)"\s*\+\s*QuoteArgument\(command\);\s*\}'
$matches = [regex]::Matches($source, $pattern)
Assert-Test ($matches.Count -eq 1) 'Extract exactly one current production command expression.'
$prefix = Decode-CppLiteral $matches[0].Groups['prefix'].Value
$suffix = Decode-CppLiteral $matches[0].Groups['suffix'].Value
$arguments = Decode-CppLiteral $matches[0].Groups['args'].Value
Assert-Test ($prefix.StartsWith('$env:PSModulePath = $PSHOME + ''\Modules'';')) 'Module path is restricted before invoking fixture code.'
Assert-Test ($suffix -ceq '; exit $LASTEXITCODE') 'The production command propagates the invoked script exit code explicitly.'
Assert-Test ($arguments -ceq ' -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ') 'Use the production PowerShell command mode and startup flags.'

$guiMatches = [regex]::Matches($source, $pattern.Replace('CommandLine\(', 'GuiCommandLine\('))
Assert-Test ($guiMatches.Count -eq 1) 'Extract exactly one production GUI command expression.'
$guiPrefix = Decode-CppLiteral $guiMatches[0].Groups['prefix'].Value
$guiSuffix = Decode-CppLiteral $guiMatches[0].Groups['suffix'].Value
$guiArguments = Decode-CppLiteral $guiMatches[0].Groups['args'].Value
Assert-Test ($guiSuffix -ceq ' -ApprovedLocalTrust -NonInteractive; exit $LASTEXITCODE') 'Only explicit GUI approval can authorize noninteractive trust.'
Assert-Test ($guiArguments -ceq ' -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ') 'GUI child disables prompts and profiles.'
Assert-Test ($guiPrefix.Contains('[Console]::OutputEncoding = [Text.Encoding]::UTF8;')) 'Captured GUI log encoding is explicit.'

function Quote-ProcessArgument([string] $Value) {
    # Fixture launcher only: .NET ProcessStartInfo.Arguments uses a command-line
    # string on both supported hosts. This is not the native CreateProcess test.
    $builder = New-Object Text.StringBuilder
    $null = $builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            $null = $builder.Append(('\' * ($slashes * 2 + 1)))
        } else {
            $null = $builder.Append(('\' * $slashes))
        }
        $slashes = 0
        $null = $builder.Append($character)
    }
    $null = $builder.Append(('\' * ($slashes * 2)))
    $null = $builder.Append('"')
    return $builder.ToString()
}
function Invoke-Fixture([string] $Name, [string] $Path, [int] $Expected, [bool] $WithoutExit = $false) {
    $literal = "'" + $Path.Replace("'", "''") + "'"
    $command = $prefix + $literal
    if (-not $WithoutExit) { $command += $suffix }
    $tokens = $null
    $errors = $null
    $null = [Management.Automation.Language.Parser]::ParseInput($command, [ref]$tokens, [ref]$errors)
    Assert-Test ($errors.Count -eq 0) ('Fixture command parses: ' + $Name)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $powerShellHost
    $start.Arguments = $arguments.TrimStart() + (Quote-ProcessArgument $command)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # This affects only the isolated child and is overwritten by the extracted
    # production prefix. No user/machine environment variable is changed.
    $start.EnvironmentVariables['PSModulePath'] = (Join-Path $testRoot 'untrusted-modules')
    $child = New-Object Diagnostics.Process
    $child.StartInfo = $start
    try {
        Assert-Test ($child.Start()) ('Start isolated child: ' + $Name)
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        if (-not $child.WaitForExit(30000)) {
            # Only this test-owned fixture process is terminated on timeout.
            $child.Kill()
            $child.WaitForExit()
            throw ('Fixture child timed out: ' + $Name)
        }
        $output = $stdout.GetAwaiter().GetResult()
        $errorOutput = $stderr.GetAwaiter().GetResult()
        $code = $child.ExitCode
        Assert-Test ($code -eq $Expected) ('Exit status for ' + $Name + ': expected ' + $Expected + ', got ' + $code + '; ' + $errorOutput)
        $records.Add([pscustomobject]@{
            name = $Name; expected_exit = $Expected; observed_exit = $code
            production_suffix_used = (-not $WithoutExit)
            stdout = $output; stderr_present = (-not [string]::IsNullOrEmpty($errorOutput))
        })
    } finally { $child.Dispose() }
}

$created = $false
try {
    $null = New-Item -ItemType Directory -Path $testRoot
    $created = $true
    foreach ($code in @(0, 1, 2, 7, 42, 255)) {
        $path = Join-Path $testRoot ('exit-' + $code + '.ps1')
        [IO.File]::WriteAllText($path, ('exit ' + $code + "`n"))
        Invoke-Fixture ('Explicit script exit ' + $code) $path $code
    }
    $quoted = Join-Path $testRoot 'quoted '' & ; dollar$ fixture.ps1'
    [IO.File]::WriteAllText($quoted, "[Console]::WriteLine('quoted path executed'); exit 7`n")
    Invoke-Fixture 'Spaces, apostrophe and shell metacharacters remain one literal script path' $quoted 7

    $failure = Join-Path $testRoot 'script-error.ps1'
    [IO.File]::WriteAllText($failure, "throw 'Expected isolated fixture error'`n")
    Invoke-Fixture 'Terminating script error' $failure 1
    Invoke-Fixture 'Missing script cannot report successful installation' (Join-Path $testRoot 'missing.ps1') 1

    $modulePath = Join-Path $testRoot 'module-path.ps1'
    [IO.File]::WriteAllText($modulePath, 'if ($env:PSModulePath -cne ($PSHOME + ''\Modules'')) { exit 91 }; exit 0')
    Invoke-Fixture 'Production prefix replaces inherited module search path' $modulePath 0

    Invoke-Fixture 'Negative control: omitting explicit exit collapses script cancellation 2 to host failure 1' (Join-Path $testRoot 'exit-2.ps1') 1 $true

    $legacyPrefix = $prefix; $legacySuffix = $suffix; $legacyArguments = $arguments
    $prefix = $guiPrefix; $suffix = $guiSuffix; $arguments = $guiArguments
    foreach ($code in @(0, 1, 2, 7, 42, 255)) {
        $path = Join-Path $testRoot ('gui-exit-' + $code + '.ps1')
        $fixture = 'param([switch]$ApprovedLocalTrust,[switch]$NonInteractive)' + "`n" +
            'if (-not $ApprovedLocalTrust -or -not $NonInteractive) { exit 91 }' + "`n" +
            'if ($env:PSModulePath -cne ($PSHOME + ''\Modules'')) { exit 92 }' + "`n" +
            '[Console]::WriteLine(''GUI fixture \u2014 captured output''); exit ' + $code
        [IO.File]::WriteAllText($path, $fixture)
        Invoke-Fixture ('GUI approved flags and explicit script exit ' + $code) $path $code
    }
    $quotedGui = Join-Path $testRoot 'gui quoted '' & ; dollar$ fixture.ps1'
    [IO.File]::WriteAllText($quotedGui, 'param([switch]$ApprovedLocalTrust,[switch]$NonInteractive) if (-not $ApprovedLocalTrust -or -not $NonInteractive) { exit 91 }; exit 7')
    Invoke-Fixture 'GUI script path with spaces, apostrophe and shell metacharacters remains one literal' $quotedGui 7
    $prefix = $legacyPrefix; $suffix = $legacySuffix; $arguments = $legacyArguments

    $report = [ordered]@{
        schema_version = 1
        suite = 'setup_powershell'
        generated_utc = [DateTime]::UtcNow.ToString('o')
        passed = $true
        cases = $records.Count
        assertions = $script:assertions
        powershell_version = $PSVersionTable.PSVersion.ToString()
        platform = [Environment]::OSVersion.Platform.ToString()
        native_installer_executed = $false
        source_sha256 = @{ 'src/setup_policy.h' = $sourceHash; 'tests/setup_powershell_tests.ps1' = $testHash }
        command_prefix = $prefix
        command_suffix = $suffix
        host_arguments = $arguments
        gui_command_prefix = $guiPrefix
        gui_command_suffix = $guiSuffix
        gui_host_arguments = $guiArguments
        scope = 'Production command literal pieces extracted from C++ source, real isolated PowerShell child processes and temporary script IO. C++ helper execution/quoting is tested separately in setup_tests.'
        limitations = @('Does not run Install.ps1 or modify certificates, ACLs, Program Files or installed application state.', 'Does not execute native CreateProcess, the setup EXE, UAC, or the fixed native child environment block.', 'Windows PowerShell 5.1 is not certified by a run under PowerShell 7; host version is recorded.')
        results = $records.ToArray()
    }
    if ($ReportPath) {
        [IO.File]::WriteAllText([IO.Path]::GetFullPath($ReportPath), ($report | ConvertTo-Json -Depth 8) + "`n")
    }
    Write-Host ('PASS: ' + $records.Count + ' setup PowerShell process scenarios; ' + $script:assertions + ' assertions; production command fragments, real exit codes, no installer execution.')
} finally {
    if ($created -and [IO.Directory]::Exists($testRoot)) {
        # All content in this unique fixture directory belongs to this test.
        [IO.Directory]::Delete($testRoot, $true)
    }
}
