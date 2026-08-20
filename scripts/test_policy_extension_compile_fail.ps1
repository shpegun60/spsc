[CmdletBinding()]
param(
    [string]$Compiler = 'g++'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$includeDir = Join-Path $repoRoot 'src\spsc'
$compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$compilerFileName = [IO.Path]::GetFileName($compilerPath)
$isMsvc = $compilerFileName -in @('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')

$scenarios = @(
    @{
        name = 'non-power-of-two Policy::allocator_alignment'
        source = Join-Path $repoRoot 'tests\compile_fail\policy_allocator_alignment_invalid.cpp'
        expected = '[spsc::alloc]: Policy::allocator_alignment must be a non-zero power of two'
        defines = @()
    },
    @{
        name = 'negative Policy::allocator_alignment'
        source = Join-Path $repoRoot 'tests\compile_fail\policy_allocator_alignment_invalid.cpp'
        expected = '[spsc::alloc]: Policy::allocator_alignment must be positive and representable as size_t'
        defines = @('SPSC_TEST_NEGATIVE_POLICY_ALIGNMENT=1')
    },
    @{
        name = 'non-integral Policy::allocator_alignment'
        source = Join-Path $repoRoot 'tests\compile_fail\policy_allocator_alignment_invalid.cpp'
        expected = '[spsc::alloc]: Policy::allocator_alignment must be an integral or enum constant'
        defines = @('SPSC_TEST_NONINTEGRAL_POLICY_ALIGNMENT=1')
    },
    @{
        name = 'incomplete custom counter contract'
        source = Join-Path $repoRoot 'tests\compile_fail\counter_contract_invalid.cpp'
        expected = '[Policy]: counter_type must satisfy the custom counter contract'
        defines = @()
    },
    @{
        name = 'direct custom policy with an incomplete counter contract'
        source = Join-Path $repoRoot 'tests\compile_fail\counter_contract_invalid.cpp'
        expected = '[SPSCbase]: PolicyT::counter_type must satisfy the custom counter contract'
        defines = @('SPSC_TEST_DIRECT_POLICY_COUNTER=1')
    },
    @{
        name = 'direct custom policy with an incomplete geometry contract'
        source = Join-Path $repoRoot 'tests\compile_fail\counter_contract_invalid.cpp'
        expected = '[CapacityCtrl<0>]: Policy::geometry_type must satisfy the custom counter contract'
        defines = @('SPSC_TEST_DIRECT_POLICY_GEOMETRY=1')
    }
)

foreach ($scenario in $scenarios) {
    if ($isMsvc) {
        $arguments = @(
            '/nologo',
            '/std:c++17',
            "/I$repoRoot",
            "/I$includeDir",
            '/Zs',
            $scenario.source
        )
        $arguments += @($scenario.defines | ForEach-Object { "/D$_" })
    } else {
        $arguments = @(
            '-std=c++17',
            "-I$repoRoot",
            "-I$includeDir",
            '-fsyntax-only',
            $scenario.source
        )
        $arguments += @($scenario.defines | ForEach-Object { "-D$_" })
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
        throw "$($scenario.name) unexpectedly compiled; the extension contract is not enforced."
    }

    $diagnosticText = [string]::Join([Environment]::NewLine, [string[]]$diagnostics)
    if ($diagnosticText -notmatch [regex]::Escape($scenario.expected)) {
        throw "$($scenario.name) failed for an unexpected reason:`n$diagnosticText"
    }
}

Write-Output "PASS: invalid custom policy/counter extensions were rejected by $compilerPath"
exit 0
