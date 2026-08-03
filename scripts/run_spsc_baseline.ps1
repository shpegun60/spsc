[CmdletBinding()]
param(
    [ValidateRange(1, 9223372036854775807)]
    [UInt64]$Items = 20000000,

    [ValidateRange(1, 1000)]
    [int]$Samples = 9,

    [ValidateRange(0, 1000)]
    [int]$Warmup = 2,

    [ValidateSet(64, 256, 1024, 4096)]
    [int]$Capacity = 1024,

    [ValidateSet('all', 'queue', 'fifo', 'policy')]
    [string]$Suite = 'all',

    [string]$Compiler = 'g++',

    # Keep the current checkout's benchmark harness while compiling it against
    # another checked-out library revision for an A/B comparison.
    [string]$LibraryRoot = '',

    [int]$ProducerCpu = -1,
    [int]$ConsumerCpu = -1,
    [switch]$NoAffinity,

    [ValidateSet('forward', 'both')]
    [string]$Directions = 'both',

    [switch]$RequireClean,

    [ValidateSet('diagnostic', 'release')]
    [string]$EvidenceClass = 'diagnostic',

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$CaptureLabel = 'diagnostic'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$releaseEvidence = ($EvidenceClass -eq 'release')
$requireCleanCapture = $RequireClean -or $releaseEvidence
if ($releaseEvidence) {
    if ($CaptureLabel -eq 'diagnostic') {
        throw 'Release evidence requires an explicit capture label such as h0, h2, or h8'
    }
    if ($NoAffinity) {
        throw 'Release evidence requires explicit or automatically resolved CPU affinity'
    }
    if ($Directions -ne 'both') {
        throw 'Release evidence requires both producer/consumer affinity directions'
    }
    if ($Items -lt 20000000 -or $Samples -lt 9 -or $Warmup -lt 2) {
        throw 'Release evidence requires at least 20,000,000 items, 9 samples, and 2 warm-ups'
    }
}

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

function Get-PowerState {
    $state = [ordered]@{
        scheme = $null
        overlay_ac = $null
        overlay_dc = $null
    }
    if ($env:OS -ne 'Windows_NT' -or
        -not (Get-Command powercfg.exe -ErrorAction SilentlyContinue)) {
        return [pscustomobject]$state
    }

    $state.scheme = ((& powercfg.exe /GETACTIVESCHEME) | Out-String).Trim()
    $overlayPath = 'HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes'
    if (Test-Path -LiteralPath $overlayPath) {
        $overlayState = Get-ItemProperty -LiteralPath $overlayPath -ErrorAction SilentlyContinue
        if ($null -ne $overlayState) {
            $acProperty = $overlayState.PSObject.Properties['ActiveOverlayAcPowerScheme']
            $dcProperty = $overlayState.PSObject.Properties['ActiveOverlayDcPowerScheme']
            if ($null -ne $acProperty) {
                $state.overlay_ac = $acProperty.Value
            }
            if ($null -ne $dcProperty) {
                $state.overlay_dc = $dcProperty.Value
            }
        }
    }
    return [pscustomobject]$state
}

$harnessRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($LibraryRoot)) {
    $libraryRoot = $harnessRoot
} else {
    if (-not (Test-Path -LiteralPath $LibraryRoot -PathType Container)) {
        throw "LibraryRoot does not exist: $LibraryRoot"
    }
    $libraryRoot = [IO.Path]::GetFullPath($LibraryRoot)
}

$benchDir = Join-Path $harnessRoot 'benchmarks'
$sourcePath = Join-Path $benchDir 'spsc_bench.cpp'
$probePath = Join-Path $benchDir 'hotpath_probe.cpp'
$rigtorpHeader = Join-Path $libraryRoot 'third_party\rigtorp_spscqueue\include\rigtorp\SPSCQueue.h'
$rigtorpExpectedCommit = '565a5149d54930463d58cb0f69b978d439555e66'
$buildDir = Join-Path $benchDir '.build'
$resultsDir = Join-Path $benchDir 'results'

if (-not (Test-Path -LiteralPath $rigtorpHeader)) {
    Invoke-Checked -File 'git' -Arguments @(
        '-C', $libraryRoot, 'submodule', 'update', '--init', '--recursive',
        '--', 'third_party/rigtorp_spscqueue'
    ) -FailureMessage 'Unable to initialize the pinned Rigtorp submodule'
}

$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
[IO.Directory]::CreateDirectory($buildDir) | Out-Null
[IO.Directory]::CreateDirectory($resultsDir) | Out-Null

$commit = (& git -C $libraryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the benchmark source revision'
}
$shortCommit = (& git -C $libraryRoot rev-parse --short=12 HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the short benchmark source revision'
}
$gitStatus = @(& git -C $libraryRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to capture git worktree state'
}
if ($requireCleanCapture -and $gitStatus.Count -ne 0) {
    throw 'A clean worktree is required for this baseline capture'
}
$harnessCommit = (& git -C $harnessRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the benchmark harness revision'
}
$harnessStatus = @(& git -C $harnessRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to capture benchmark harness worktree state'
}
if ($requireCleanCapture -and $harnessStatus.Count -ne 0) {
    throw 'A clean benchmark harness worktree is required for this baseline capture'
}
$rigtorpState = @(& git -C $libraryRoot submodule status -- third_party/rigtorp_spscqueue)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to capture the pinned Rigtorp gitlink'
}
$rigtorpCommit = (& git -C (Join-Path $libraryRoot 'third_party\rigtorp_spscqueue') rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $rigtorpCommit -ne $rigtorpExpectedCommit) {
    throw "Rigtorp must be pinned at $rigtorpExpectedCommit"
}
$rigtorpWorktreeStatus = @(& git -C (Join-Path $libraryRoot 'third_party\rigtorp_spscqueue') status --porcelain)
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
    "-I$libraryRoot",
    "-I$(Join-Path $libraryRoot 'src')",
    "-I$(Join-Path $libraryRoot 'third_party\rigtorp_spscqueue\include')"
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
        $affinity = 'auto'
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
    '--directions', $Directions,
    '--output', $resultPath,
    '--commit', $commit
)
$powerBefore = Get-PowerState
$processPriority = (Get-Process -Id $PID).PriorityClass.ToString()
Invoke-Checked -File $binaryPath -Arguments $runArgs -FailureMessage 'Benchmark execution failed'
$powerAfter = Get-PowerState
if ($releaseEvidence -and
    (($powerBefore | ConvertTo-Json -Compress) -ne
     ($powerAfter | ConvertTo-Json -Compress))) {
    throw 'Release evidence requires an unchanged power scheme and overlay during capture'
}

$metadataRecord = Get-Content -LiteralPath $resultPath -TotalCount 1 | ConvertFrom-Json
if ($metadataRecord.kind -ne 'metadata') {
    throw 'Benchmark result did not begin with a metadata record'
}
$resultRecords = @(Get-Content -LiteralPath $resultPath | ForEach-Object { $_ | ConvertFrom-Json })
$sampleRecords = @($resultRecords | Where-Object { $_.kind -eq 'sample' })
$firstSampleRecord = $sampleRecords | Select-Object -First 1
if ($null -eq $firstSampleRecord) {
    throw 'Benchmark result did not contain a sample record'
}
$resolvedAffinities = @($sampleRecords |
    Group-Object direction |
    ForEach-Object {
        $sample = $_.Group | Select-Object -First 1
        [ordered]@{
            direction = $_.Name
            producer_cpu = $sample.affinity.producer_cpu
            consumer_cpu = $sample.affinity.consumer_cpu
        }
    })
$actualDirections = @($resolvedAffinities | ForEach-Object { $_.direction } | Sort-Object -Unique)
if ($Directions -eq 'both' -and $affinity -ne 'none') {
    if ($actualDirections.Count -ne 2 -or
        $actualDirections -notcontains 'forward' -or
        $actualDirections -notcontains 'reverse') {
        throw 'Bidirectional benchmark output did not contain forward and reverse affinity samples'
    }
}
if ($releaseEvidence) {
    foreach ($sample in $sampleRecords) {
        if ($sample.affinity.producer_cpu -eq $sample.affinity.consumer_cpu -or
            -not $sample.affinity.producer_applied -or
            -not $sample.affinity.consumer_applied) {
            throw 'Release evidence requires applied affinity on two distinct logical CPUs'
        }
    }
}
if (@($sampleRecords | Where-Object { -not $_.verified }).Count -ne 0) {
    throw 'Benchmark result contains an unverified sample'
}
$comparisonRecords = @($resultRecords | Where-Object { $_.kind -eq 'comparison_summary' })
if (($Suite -eq 'all' -or $Suite -eq 'queue') -and $comparisonRecords.Count -ne 2) {
    throw 'Queue benchmark must emit one comparison summary per workload'
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
    format_version = 4
    captured_at_local = (Get-Date).ToString('o')
    evidence = [ordered]@{
        classification = $EvidenceClass
        capture_label = $CaptureLabel
        clean_library_required = $requireCleanCapture
        clean_harness_required = $requireCleanCapture
        minimum_release_protocol = if ($releaseEvidence) {
            'items>=20000000;samples>=9;warmup>=2;distinct-bidirectional-affinity'
        } else {
            $null
        }
    }
    git = [ordered]@{
        commit = $commit
        status_porcelain = $gitStatus
        rigtorp_submodule = $rigtorpState
        library_root = $libraryRoot
    }
    harness = [ordered]@{
        root = $harnessRoot
        commit = $harnessCommit
        status_porcelain = $harnessStatus
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
        affinity_selection = $metadataRecord.affinity_selection
        affinity_directions_requested = $Directions
        affinity_resolved = $resolvedAffinities
        power_before = $powerBefore
        power_after = $powerAfter
        inherited_process_priority = $processPriority
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
        retry_backoff = $metadataRecord.retry_backoff
        worker_lifecycle = $metadataRecord.worker_lifecycle
        thread_cycle_counter = $metadataRecord.thread_cycle_counter
        ranking_gate = $metadataRecord.ranking_gate
        comparison_summaries = $comparisonRecords
        shadow_indices_enabled = 1
        shadow_allow_32bit = 0
        queue_comparison_protocol = 'persistent-workers;paired-alternating-order;bidirectional-affinity'
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

Write-Host "Benchmark results: $resultPath"
Write-Host "Manifest:         $manifestPath"
Write-Host "Assembly:         $assemblyPath"
foreach ($comparison in $comparisonRecords) {
    Write-Host ("Comparison {0}: {1} ({2})" -f
        $comparison.workload, $comparison.status, $comparison.reason)
}
