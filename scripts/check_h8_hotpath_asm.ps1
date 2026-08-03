[CmdletBinding()]
param(
    [string]$Compiler = 'cl'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$probe = Join-Path $repoRoot 'benchmarks\hotpath_probe.cpp'
$workDir = Join-Path ([IO.Path]::GetTempPath()) ("spsc-h8-asm-" + [Guid]::NewGuid().ToString('N'))
$assembly = Join-Path $workDir 'hotpath.asm'
$object = Join-Path $workDir 'hotpath.obj'

function Get-FunctionBody {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Lines,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    $start = -1
    for ($index = 0; $index -lt $Lines.Count; ++$index) {
        if ($Lines[$index] -match ("^" + [regex]::Escape($Name) + "\s+PROC\b")) {
            $start = $index
            break
        }
    }
    if ($start -lt 0) {
        throw "H8 assembly check failed: $Name was not emitted"
    }

    $body = New-Object System.Collections.Generic.List[string]
    for ($index = $start; $index -lt $Lines.Count; ++$index) {
        $body.Add($Lines[$index])
        if ($Lines[$index] -match ("^" + [regex]::Escape($Name) + "\s+ENDP\b")) {
            return $body.ToArray()
        }
    }
    throw "H8 assembly check failed: $Name has no ENDP marker"
}

function Require-Count {
    param(
        [Parameter(Mandatory = $true)] [int]$Expected,
        [Parameter(Mandatory = $true)] [int]$Actual,
        [Parameter(Mandatory = $true)] [string]$Description,
        [Parameter(Mandatory = $true)] [string[]]$Body
    )

    if ($Actual -ne $Expected) {
        $formattedBody = $Body -join [Environment]::NewLine
        throw "H8 assembly check failed: expected $Expected $Description, got $Actual`n--- probe body ---`n$formattedBody"
    }
}

New-Item -ItemType Directory -Path $workDir -ErrorAction Stop | Out-Null
try {
    $compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
    $compilerPath = $compilerCommand.Source
    $includeRoot = "/I$repoRoot"
    $includeSrc = "/I$(Join-Path $repoRoot 'src')"
    $includeRigtorp = "/I$(Join-Path $repoRoot 'third_party\rigtorp_spscqueue\include')"

    & $compilerPath /nologo /std:c++17 /O2 /DNDEBUG `
        /DSPSC_ENABLE_SHADOW_INDICES=1 /DSPSC_SHADOW_ALLOW_32BIT=0 `
        $includeRoot $includeSrc $includeRigtorp `
        /FAs "/Fa$assembly" "/Fo$object" /c $probe
    if ($LASTEXITCODE -ne 0) {
        throw "MSVC hot-path assembly generation failed (exit $LASTEXITCODE)"
    }

    if (-not (Test-Path -LiteralPath $assembly) -or
        (Get-Item -LiteralPath $assembly).Length -eq 0) {
        $artifacts = @(Get-ChildItem -LiteralPath $workDir -Force |
            ForEach-Object { "$($_.Name) ($($_.Length) bytes)" }) -join ', '
        throw "MSVC hot-path assembly was not written to $assembly. Artifacts: $artifacts"
    }

    # MSVC's listing can begin with an empty line; remove it before binding to
    # the mandatory string-array parameter below.
    $lines = @(Get-Content -LiteralPath $assembly | Where-Object { $_.Length -ne 0 })
    $producerBody = Get-FunctionBody -Lines $lines -Name 'spsc_fifo_producer'
    $consumerBody = Get-FunctionBody -Lines $lines -Name 'spsc_fifo_consumer'
    $queueProducerBody = Get-FunctionBody -Lines $lines -Name 'spsc_queue_producer'
    $queueConsumerBody = Get-FunctionBody -Lines $lines -Name 'spsc_queue_consumer'

    # FIFO head is at offset 0. Count only a register destination and memory
    # source so the final publication store is intentionally excluded.
    $producerHeadLoads = @($producerBody | Where-Object {
        $_ -match '^\s*mov\s+\w+,\s+QWORD PTR \[\w+\]\s*(?:;.*)?$'
    }).Count

    # In the CFA layout tail is at offset 128. The public consumer probe has
    # one tail snapshot for try_front() and one for pop().
    $consumerTailLoads = @($consumerBody | Where-Object {
        $_ -match '^\s*mov\s+\w+,\s+QWORD PTR \[\w+\s*\+\s*(?:128|80H)\]\s*(?:;.*)?$'
    }).Count

    # queue_base's allocation-state byte precedes the cache-line-aligned
    # SPSCbase, so queue head/tail are at offsets 64/192 in this probe.
    $queueProducerHeadLoads = @($queueProducerBody | Where-Object {
        $_ -match '^\s*mov\s+\w+,\s+QWORD PTR \[\w+\s*\+\s*(?:64|40H)\]\s*(?:;.*)?$'
    }).Count

    $queueConsumerTailLoads = @($queueConsumerBody | Where-Object {
        $_ -match '^\s*mov\s+\w+,\s+QWORD PTR \[\w+\s*\+\s*(?:192|0?C0H)\]\s*(?:;.*)?$'
    }).Count

    Require-Count -Expected 1 -Actual $producerHeadLoads `
        -Description 'producer head load' -Body $producerBody
    Require-Count -Expected 2 -Actual $consumerTailLoads `
        -Description 'consumer tail loads' -Body $consumerBody
    Require-Count -Expected 1 -Actual $queueProducerHeadLoads `
        -Description 'queue producer head load' -Body $queueProducerBody
    Require-Count -Expected 2 -Actual $queueConsumerTailLoads `
        -Description 'queue consumer tail loads' -Body $queueConsumerBody

    Write-Output 'PASS: H8 hot-path assembly (MSVC): fifo=1/2 queue=1/2 owner loads'
} finally {
    if (Test-Path -LiteralPath $workDir) {
        Remove-Item -LiteralPath $workDir -Recurse -Force
    }
}
