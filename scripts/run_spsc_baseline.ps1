[CmdletBinding()]
param(
    [ValidateRange(1, 9223372036854775807)]
    [UInt64]$Items = 2000000,

    [ValidateRange(1, 1000)]
    [int]$Samples = 5,

    [ValidateRange(0, 1000)]
    [int]$Warmup = 1,

    [ValidateSet(64, 256, 1024, 4096)]
    [int]$Capacity = 1024,

    [ValidateSet('all', 'queue', 'fifo', 'policy')]
    [string]$Suite = 'all',

    [string]$Compiler = 'g++',

    [int]$ProducerCpu = -1,
    [int]$ConsumerCpu = -1,
    [switch]$NoAffinity,
    [switch]$RequireClean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$benchDir = Join-Path $repoRoot 'benchmarks'
$sourcePath = Join-Path $benchDir 'spsc_bench.cpp'
$probePath = Join-Path $benchDir 'hotpath_probe.cpp'
$rigtorpHeader = Join-Path $repoRoot 'third_party\rigtorp_spscqueue\include\rigtorp\SPSCQueue.h'
$rigtorpExpectedCommit = '565a5149d54930463d58cb0f69b978d439555e66'
$buildDir = Join-Path $benchDir '.build'
$resultsDir = Join-Path $benchDir 'results'

if (-not (Test-Path -LiteralPath $rigtorpHeader)) {
    Invoke-Checked -File 'git' -Arguments @(
        '-C', $repoRoot, 'submodule', 'update', '--init', '--recursive',
        '--', 'third_party/rigtorp_spscqueue'
    ) -FailureMessage 'Unable to initialize the pinned Rigtorp submodule'
}

$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
[IO.Directory]::CreateDirectory($buildDir) | Out-Null
[IO.Directory]::CreateDirectory($resultsDir) | Out-Null

$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the benchmark source revision'
}
$shortCommit = (& git -C $repoRoot rev-parse --short=12 HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the short benchmark source revision'
}
$gitStatus = @(& git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to capture git worktree state'
}
if ($RequireClean -and $gitStatus.Count -ne 0) {
    throw 'A clean worktree is required for this baseline capture'
}
$rigtorpState = @(& git -C $repoRoot submodule status -- third_party/rigtorp_spscqueue)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to capture the pinned Rigtorp gitlink'
}
$rigtorpCommit = (& git -C (Join-Path $repoRoot 'third_party\rigtorp_spscqueue') rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $rigtorpCommit -ne $rigtorpExpectedCommit) {
    throw "Rigtorp must be pinned at $rigtorpExpectedCommit"
}
$rigtorpWorktreeStatus = @(& git -C (Join-Path $repoRoot 'third_party\rigtorp_spscqueue') status --porcelain)
if ($LASTEXITCODE -ne 0 -or $rigtorpWorktreeStatus.Count -ne 0 -or
    $rigtorpState.Count -ne 1 -or $rigtorpState[0] -match '^[+-]') {
    throw 'Rigtorp submodule must match its clean pinned gitlink'
}

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$extension = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
$binaryPath = Join-Path $buildDir "spsc_bench$extension"
$assemblyPath = Join-Path $resultsDir "$timestamp-$shortCommit.hotpath.s"
$resultPath = Join-Path $resultsDir "$timestamp-$shortCommit.jsonl"
$manifestPath = Join-Path $resultsDir "$timestamp-$shortCommit.manifest.json"

$compileFlags = @(
    '-std=c++17',
    '-O3',
    '-DNDEBUG',
    '-march=native',
    '-pthread',
    '-DSPSC_ENABLE_SHADOW_INDICES=1',
    '-DSPSC_SHADOW_ALLOW_32BIT=0'
)

# Rigtorp v1.1 uses hardware_destructive_interference_size when the standard
# library exposes it. Pin GCC's value so a later tuning default cannot silently
# alter the comparator layout in a reproduced capture.
if ($compilerCommand.Name -match 'g\+\+') {
    $compileFlags += '--param=destructive-interference-size=64'
}

$includeFlags = @(
    "-I$repoRoot",
    "-I$(Join-Path $repoRoot 'src')",
    "-I$(Join-Path $repoRoot 'third_party\rigtorp_spscqueue\include')"
)
$compileArgs = @()
$compileArgs += $compileFlags
$compileArgs += $includeFlags
$compileArgs += @('-o', $binaryPath, $sourcePath)
Invoke-Checked -File $compilerPath -Arguments $compileArgs -FailureMessage 'Benchmark compilation failed'

$assemblyArgs = @()
$assemblyArgs += $compileFlags
$assemblyArgs += $includeFlags
$assemblyArgs += @('-S', '-o', $assemblyPath, $probePath)
Invoke-Checked -File $compilerPath -Arguments $assemblyArgs -FailureMessage 'Hot-path assembly generation failed'

$logicalProcessors = [Environment]::ProcessorCount
$affinity = 'none'
if (-not $NoAffinity) {
    if (($ProducerCpu -ge 0) -xor ($ConsumerCpu -ge 0)) {
        throw 'Specify both -ProducerCpu and -ConsumerCpu, or neither'
    }
    if ($ProducerCpu -ge 0) {
        if ($ProducerCpu -ge $logicalProcessors -or $ConsumerCpu -ge $logicalProcessors) {
            throw "Requested affinity is outside the $logicalProcessors logical processors reported by this host"
        }
        $affinity = "$ProducerCpu,$ConsumerCpu"
    } elseif ($logicalProcessors -ge 2) {
        $affinity = '0,1'
    }
}

$env:SPSC_BENCH_BUILD_FLAGS = (($compileFlags + $includeFlags) -join ' ')
$env:SPSC_BENCH_COMPILER_PATH = $compilerPath
$runArgs = @(
    '--items', [string]$Items,
    '--samples', [string]$Samples,
    '--warmup', [string]$Warmup,
    '--capacity', [string]$Capacity,
    '--suite', $Suite,
    '--affinity', $affinity,
    '--output', $resultPath,
    '--commit', $commit
)
Invoke-Checked -File $binaryPath -Arguments $runArgs -FailureMessage 'Benchmark execution failed'

$metadataRecord = Get-Content -LiteralPath $resultPath -TotalCount 1 | ConvertFrom-Json
if ($metadataRecord.kind -ne 'metadata') {
    throw 'Benchmark result did not begin with a metadata record'
}

$compilerVersion = @(& $compilerPath '--version') | Select-Object -First 2
$cpuInfo = @()
if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
    $cpuInfo = @(Get-CimInstance Win32_Processor | ForEach-Object {
        [ordered]@{
            name = $_.Name
            manufacturer = $_.Manufacturer
            max_clock_mhz = $_.MaxClockSpeed
            cores = $_.NumberOfCores
            logical_processors = $_.NumberOfLogicalProcessors
        }
    })
}

$manifest = [ordered]@{
    format_version = 1
    captured_at_local = (Get-Date).ToString('o')
    git = [ordered]@{
        commit = $commit
        status_porcelain = $gitStatus
        rigtorp_submodule = $rigtorpState
    }
    compiler = [ordered]@{
        path = $compilerPath
        version = $compilerVersion
        flags = $compileFlags
        includes = $includeFlags
    }
    host = [ordered]@{
        os = [Environment]::OSVersion.VersionString
        logical_processors = $logicalProcessors
        processors = $cpuInfo
        affinity_requested = $affinity
    }
    benchmark = [ordered]@{
        suite = $Suite
        items = $Items
        samples = $Samples
        warmup = $Warmup
        capacity = $Capacity
        boundary_batch_size = $metadataRecord.boundary_batch_size
        payload_layout = $metadataRecord.payload_layout
        type_layout = $metadataRecord.type_layout
        spsc_cacheline_bytes = $metadataRecord.spsc_cacheline_bytes
        shadow_indices_enabled = 1
        shadow_allow_32bit = 0
    }
    artifacts = [ordered]@{
        jsonl = $resultPath
        jsonl_sha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash
        assembly = $assemblyPath
        assembly_sha256 = (Get-FileHash -LiteralPath $assemblyPath -Algorithm SHA256).Hash
        rigtorp_header_sha256 = (Get-FileHash -LiteralPath $rigtorpHeader -Algorithm SHA256).Hash
    }
}

$encoding = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), $encoding)

Write-Host "Baseline results: $resultPath"
Write-Host "Manifest:         $manifestPath"
Write-Host "Assembly:         $assemblyPath"
