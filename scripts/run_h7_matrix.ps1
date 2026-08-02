[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Configuration = 'Both',

    [ValidateSet('all', 'shadow_off', 'shadow_on', 'shadow_heur', 'cxx20_span')]
    [string]$Variant = 'all',

    [ValidateSet('all', 'buffer_pool', 'chunk', 'fifo', 'fifo_view', 'latest', 'pool', 'pool_view', 'queue', 'typed_pool')]
    [string]$Suite = 'all',

    [string]$Qmake = 'qmake',
    [string]$Make = 'mingw32-make',
    [string]$Compiler = 'g++',

    [ValidateRange(1, 64)]
    [int]$Jobs = 2,

    [switch]$BuildLauncher,
    [switch]$KeepBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$allSuites = @('buffer_pool', 'chunk', 'fifo', 'fifo_view', 'latest', 'pool', 'pool_view', 'queue', 'typed_pool')

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)] [string]$File,
        [Parameter(Mandatory = $true)] [string[]]$Arguments,
        [Parameter(Mandatory = $true)] [string]$FailureMessage
    )

    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit $LASTEXITCODE)"
    }
}

function Assert-OwnedTemporaryDirectory {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith("$tempRoot\", [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($fullPath) -notmatch '^spsc-h7-[0-9a-f]{32}$') {
        throw "Refusing to remove a directory not owned by H7: $fullPath"
    }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$qmakePath = (Get-Command -Name $Qmake -ErrorAction Stop).Source
$makePath = (Get-Command -Name $Make -ErrorAction Stop).Source
$compilerPath = (Get-Command -Name $Compiler -ErrorAction Stop).Source
$qtBin = Split-Path -Parent $qmakePath
$makeBin = Split-Path -Parent $makePath
$compilerBin = Split-Path -Parent $compilerPath
$env:PATH = "$qtBin;$compilerBin;$makeBin;$env:PATH"
$workDir = Join-Path ([IO.Path]::GetTempPath()) ("spsc-h7-" + [Guid]::NewGuid().ToString('N'))

$projects = @{
    shadow_off = 'qmake\test_shadow_off.pro'
    shadow_on = 'qmake\test_shadow_on.pro'
    shadow_heur = 'qmake\test_shadow_heur.pro'
    cxx20_span = 'qmake\test_cxx20_span.pro'
    launcher = 'qmake\launcher.pro'
}

if ($Configuration -eq 'Both') {
    $configurations = @('Debug', 'Release')
} else {
    $configurations = @($Configuration)
}
if ($Variant -eq 'all') {
    $variants = @('shadow_off', 'shadow_on', 'shadow_heur', 'cxx20_span')
} else {
    $variants = @($Variant)
}
if ($BuildLauncher) {
    $variants += 'launcher'
}
if ($Suite -eq 'all') {
    $selectedSuites = $allSuites
} else {
    $selectedSuites = @($Suite)
}

New-Item -ItemType Directory -Path $workDir -ErrorAction Stop | Out-Null

try {
    Write-Host "[H7 matrix] qmake: $qmakePath"
    Write-Host "[H7 matrix] make:  $makePath"
    Write-Host "[H7 matrix] compiler: $compilerPath"

    foreach ($configurationName in $configurations) {
        $configurationToken = $configurationName.ToLowerInvariant()
        $otherConfigurationToken = if ($configurationToken -eq 'debug') { 'release' } else { 'debug' }

        foreach ($variantName in $variants) {
            $projectFile = Join-Path $repoRoot $projects[$variantName]
            if (-not (Test-Path -LiteralPath $projectFile -PathType Leaf)) {
                throw "Missing qmake target: $projectFile"
            }

            $buildDir = Join-Path $workDir "$configurationToken-$variantName"
            $qmakeDir = Join-Path $buildDir 'qmake'
            New-Item -ItemType Directory -Path $qmakeDir -ErrorAction Stop | Out-Null

            Write-Host "[H7 matrix] build config=$configurationToken variant=$variantName"
            Push-Location $qmakeDir
            try {
                Invoke-Checked -File $qmakePath -Arguments @(
                    '-o', 'Makefile', $projectFile,
                    "CONFIG+=$configurationToken", "CONFIG-=$otherConfigurationToken",
                    "QMAKE_CXX=$compilerPath", "QMAKE_LINK=$compilerPath"
                ) -FailureMessage "qmake failed for $configurationToken/$variantName"

                # Qt 6.4 can otherwise compile a source-MOC user before the
                # generated file exists in a pristine parallel build.
                Invoke-Checked -File $makePath -Arguments @('-f', 'Makefile', 'mocables') -FailureMessage "MOC generation failed for $configurationToken/$variantName"

                $makeArguments = @('-f', 'Makefile')
                if ([IO.Path]::GetFileName($makePath) -match '(^|-)make(\.exe)?$') {
                    $makeArguments += "-j$Jobs"
                }
                Invoke-Checked -File $makePath -Arguments $makeArguments -FailureMessage "build failed for $configurationToken/$variantName"
            } finally {
                Pop-Location
            }

            $targetName = if ($variantName -eq 'launcher') { 'spsc_launcher' } else { "spsc_test_$variantName" }
            $executable = Join-Path $buildDir "bin\$configurationToken\$targetName.exe"
            if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
                throw "Missing runner: $executable"
            }

            if ($variantName -eq 'launcher') {
                Write-Host "[H7 matrix] built config=$configurationToken variant=launcher"
                continue
            }

            foreach ($suiteName in $selectedSuites) {
                Write-Host "[H7 matrix] run config=$configurationToken variant=$variantName suite=$suiteName"
                Invoke-Checked -File $executable -Arguments @('--run-suite', $suiteName) -FailureMessage "test failed for $configurationToken/$variantName/$suiteName"
            }
        }
    }

    Write-Host 'PASS: H7 qmake matrix'
} finally {
    if ($KeepBuild) {
        Write-Host "Kept H7 matrix build directory: $workDir"
    } elseif (Test-Path -LiteralPath $workDir) {
        Assert-OwnedTemporaryDirectory -Path $workDir
        Remove-Item -LiteralPath $workDir -Recurse -Force
    }
}
