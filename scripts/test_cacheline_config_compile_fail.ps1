[CmdletBinding()]
param(
    [string]$Compiler = 'g++'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repoRoot 'tests\compile_fail\cacheline_zero.cpp'
$includeDir = Join-Path $repoRoot 'src\spsc'
$compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$isMsvc = [IO.Path]::GetFileName($compilerPath).Equals('cl.exe', [StringComparison]::OrdinalIgnoreCase)

$scenarios = @(
    @{
        name = 'SPSC_CACHELINE_MIN=0'
        define = 'SPSC_CACHELINE_MIN=0'
        expected = 'SPSC_CACHELINE_MIN must be a non-zero power-of-two'
    },
    @{
        name = 'SPSC_FORCE_CACHELINE=0'
        define = 'SPSC_FORCE_CACHELINE=0'
        expected = 'SPSC_CACHELINE_BYTES must be non-zero'
    },
    @{
        name = 'SPSC_CACHELINE_BYTES=0'
        define = 'SPSC_CACHELINE_BYTES=0'
        expected = 'SPSC_CACHELINE_BYTES must be non-zero'
    }
)

foreach ($scenario in $scenarios) {
    if ($isMsvc) {
        $arguments = @(
            '/nologo',
            '/std:c++17',
            "/D$($scenario.define)",
            "/I$repoRoot",
            "/I$includeDir",
            '/Zs',
            $source
        )
    } else {
        $arguments = @(
            '-std=c++17',
            "-D$($scenario.define)",
            "-I$repoRoot",
            "-I$includeDir",
            '-fsyntax-only',
            $source
        )
    }

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $diagnostics = & $compilerPath @arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if ($exitCode -eq 0) {
        throw "$($scenario.name) unexpectedly compiled; the zero-value rejection is broken."
    }

    $diagnosticText = [string]::Join([Environment]::NewLine, [string[]]$diagnostics)
    if ($diagnosticText -notmatch [regex]::Escape($scenario.expected)) {
        throw "$($scenario.name) failed for an unexpected reason:`n$diagnosticText"
    }
}

Write-Output "PASS: zero-valued cacheline configurations were rejected by $compilerPath"
exit 0
