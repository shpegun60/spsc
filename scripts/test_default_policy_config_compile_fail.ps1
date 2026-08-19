[CmdletBinding()]
param(
    [string]$Compiler = 'g++'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repoRoot 'tests\compile_fail\default_policy_invalid.cpp'
$includeDir = Join-Path $repoRoot 'src\spsc'
$compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$compilerFileName = [IO.Path]::GetFileName($compilerPath)
$isMsvc = $compilerFileName -in @('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')

if ($isMsvc) {
    $arguments = @(
        '/nologo',
        '/std:c++17',
        '/DSPSC_DEFAULT_POLICY_ATOMIC=2',
        "/I$repoRoot",
        "/I$includeDir",
        '/Zs',
        $source
    )
} else {
    $arguments = @(
        '-std=c++17',
        '-DSPSC_DEFAULT_POLICY_ATOMIC=2',
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
    throw 'SPSC_DEFAULT_POLICY_ATOMIC=2 unexpectedly compiled; the legacy override validation is broken.'
}

$diagnosticText = [string]::Join([Environment]::NewLine, [string[]]$diagnostics)
$expected = 'SPSC_DEFAULT_POLICY_ATOMIC must be 0 or 1'
if ($diagnosticText -notmatch [regex]::Escape($expected)) {
    throw "Invalid default-policy override failed for an unexpected reason:`n$diagnosticText"
}

Write-Output "PASS: invalid default-policy override was rejected by $compilerPath"
exit 0
