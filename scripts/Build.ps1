[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [ValidateRange(1, 2)][int] $Parallel = 2
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'

$vswhere = Get-Command vswhere.exe -ErrorAction SilentlyContinue
if ($vswhere) { $vswherePath = $vswhere.Source }
else { $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe' }
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw 'Visual Studio Installer discovery is unavailable. Install Visual Studio with Desktop development with C++ and a Windows SDK.'
}
$visualStudio = @(& $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or $visualStudio.Count -ne 1) {
    throw 'A Visual Studio installation with native x64 C++ tools is required.'
}
$installation = $visualStudio[0]
$majorVersion = ([version]$installation.installationVersion).Major
$generator = switch ($majorVersion) {
    18 { 'Visual Studio 18 2026' }
    17 { 'Visual Studio 17 2022' }
    default { throw 'Use Visual Studio 2022 or Visual Studio 2026 with native x64 C++ tools.' }
}

# Prefer a usable PATH CMake; fall back to the installed Visual Studio copy.
# Query capabilities instead of assuming an older PATH version knows VS 2026.
$candidates = New-Object 'System.Collections.Generic.List[string]'
$pathCmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($pathCmake) { $candidates.Add($pathCmake.Source) }
$candidates.Add((Join-Path $installation.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
if ($env:ProgramFiles) { $candidates.Add((Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe')) }
$cmake = $null
foreach ($candidate in @($candidates | Select-Object -Unique)) {
    if (-not (Test-Path -LiteralPath $candidate)) { continue }
    try {
        $capabilities = & $candidate -E capabilities | ConvertFrom-Json
        if ($LASTEXITCODE -eq 0 -and @($capabilities.generators | Where-Object { $_.name -eq $generator }).Count -gt 0) {
            $cmake = $candidate
            break
        }
    } catch { continue }
}
if (-not $cmake) { throw ('CMake with support for ' + $generator + ' is required. Install the Visual Studio CMake tools or a current CMake release.') }
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) { throw 'The selected CMake installation has no matching CTest executable.' }

& $cmake -S $projectRoot -B $buildDirectory -G $generator -A x64 '-DBUILD_TESTING=ON' '-DGLIDEZOOM_BUILD_APP=ON'
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed. Check the C++ workload and Windows SDK. An existing build directory must use the same generator.' }
& $cmake --build $buildDirectory --config $Configuration --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed; no output binary was copied.' }
& $ctest --test-dir $buildDirectory -C $Configuration --output-on-failure --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw 'Component, input, shader, or mocked installer tests failed; no output binary was copied.' }

$binaryDirectory = Join-Path $projectRoot 'bin'
$null = New-Item -ItemType Directory -Force -Path $binaryDirectory
Copy-Item -LiteralPath (Join-Path $buildDirectory ($Configuration + '\GlideZoom.exe')) -Destination (Join-Path $binaryDirectory 'GlideZoom.exe') -Force
Copy-Item -LiteralPath (Join-Path $buildDirectory ($Configuration + '\GlideZoom-Uninstall.exe')) -Destination (Join-Path $binaryDirectory 'GlideZoom-Uninstall.exe') -Force
Copy-Item -LiteralPath (Join-Path $buildDirectory ($Configuration + '\GlideZoom-Setup.exe')) -Destination (Join-Path $projectRoot 'GlideZoom-Setup.exe') -Force
Write-Host 'Build and tests passed. GlideZoom-Setup.exe and bin\GlideZoom.exe have been updated.'
