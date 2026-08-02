[CmdletBinding()]
param(
    [string]$Compiler = 'cl'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repoRoot 'tests\standalone\h6_32bit_shadow_matrix.cpp'
$includeDir = Join-Path $repoRoot 'src\spsc'
$compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$isMsvc = [IO.Path]::GetFileName($compilerPath).Equals('cl.exe', [StringComparison]::OrdinalIgnoreCase)
$workDir = Join-Path ([IO.Path]::GetTempPath()) ("spsc-h6-32bit-" + [Guid]::NewGuid().ToString('N'))

New-Item -ItemType Directory -Path $workDir -ErrorAction Stop | Out-Null

try {
    foreach ($allow32Bit in 0, 1) {
        $exe = Join-Path $workDir "shadow_allow_32bit_$allow32Bit.exe"

        if ($isMsvc) {
            $arguments = @(
                '/nologo',
                '/std:c++17',
                '/EHsc',
                "/I$repoRoot",
                "/I$includeDir",
                '/DSPSC_ENABLE_SHADOW_INDICES=1',
                "/DSPSC_SHADOW_ALLOW_32BIT=$allow32Bit",
                $source,
                "/Fe$exe"
            )
        } else {
            $arguments = @(
                '-m32',
                '-std=c++17',
                "-I$repoRoot",
                "-I$includeDir",
                '-DSPSC_ENABLE_SHADOW_INDICES=1',
                "-DSPSC_SHADOW_ALLOW_32BIT=$allow32Bit",
                $source,
                '-o',
                $exe
            )
        }

        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $buildOutput = & $compilerPath @arguments 2>&1
            $buildExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        if ($buildExitCode -ne 0) {
            throw "32-bit build failed for SPSC_SHADOW_ALLOW_32BIT=${allow32Bit}:`n$buildOutput"
        }

        $ErrorActionPreference = 'Continue'
        try {
            $runOutput = & $exe 2>&1
            $runExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        if ($runExitCode -ne 0) {
            throw "32-bit runtime test failed for SPSC_SHADOW_ALLOW_32BIT=${allow32Bit}:`n$runOutput"
        }

        Write-Output "PASS: genuine 32-bit shadow matrix allow_32bit=$allow32Bit"
    }
} finally {
    if (Test-Path -LiteralPath $workDir) {
        Remove-Item -LiteralPath $workDir -Recurse -Force
    }
}
