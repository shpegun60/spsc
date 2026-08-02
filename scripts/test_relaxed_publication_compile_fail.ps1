[CmdletBinding()]
param(
    [string]$Compiler = 'g++'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repoRoot 'tests\compile_fail\relaxed_publication.cpp'
$includeDir = Join-Path $repoRoot 'src\spsc'
$compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$isMsvc = [IO.Path]::GetFileName($compilerPath).Equals('cl.exe', [StringComparison]::OrdinalIgnoreCase)

if ($isMsvc) {
    $arguments = @(
        '/nologo',
        '/std:c++17',
        "/I$repoRoot",
        "/I$includeDir",
        '/Zs',
        $source
    )
} else {
    $arguments = @(
        '-std=c++17',
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
    throw 'Relaxed publication unexpectedly compiled; the rejection target is broken.'
}

$diagnosticText = [string]::Join([Environment]::NewLine, [string[]]$diagnostics)
if ($diagnosticText -notmatch 'AtomicCounter: SPSC payload publication requires acquire/seq_cst loads') {
    throw "Relaxed publication failed for an unexpected reason:`n$diagnosticText"
}

Write-Output "PASS: relaxed publication was rejected by $compilerPath"
