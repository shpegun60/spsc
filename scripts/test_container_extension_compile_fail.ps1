[CmdletBinding()]
param(
    [string]$Compiler = 'g++'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$includeDir = Join-Path $repoRoot 'src\spsc'
$source = Join-Path $repoRoot 'tests\compile_fail\container_extension_contract_invalid.cpp'
$compilerCommand = Get-Command -Name $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$compilerFileName = [IO.Path]::GetFileName($compilerPath)
$isMsvc = $compilerFileName -in @('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')

$scenarios = @(
    @{
        name = 'allocator size_type narrower than reg'
        expected = '[spsc::queue]: allocator size_type must represent the reg domain.'
        defines = @()
    },
    @{
        name = 'throwing allocator default constructor'
        expected = '[spsc::queue]: allocator must be nothrow default-constructible.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_DEFAULT=1')
    },
    @{
        name = 'throwing allocator in dynamic chunk no-exceptions mode'
        expected = '[spsc::chunk]: no-exceptions mode requires allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_CHUNK=1')
    },
    @{
        name = 'throwing allocator in dynamic fifo no-exceptions mode'
        expected = '[spsc::fifo]: no-exceptions mode requires allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_FIFO=1')
    },
    @{
        name = 'throwing allocator in allocation-backed static queue no-exceptions mode'
        expected = '[spsc::queue]: no-exceptions mode requires allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_QUEUE=1')
    },
    @{
        name = 'throwing byte allocator in static-depth pool no-exceptions mode'
        expected = '[spsc::pool]: no-exceptions mode requires byte allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_STATIC_POOL=1')
    },
    @{
        name = 'throwing slot allocator in dynamic pool no-exceptions mode'
        expected = '[spsc::pool]: no-exceptions mode requires slot allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_DYNAMIC_POOL=1')
    },
    @{
        name = 'throwing object allocator in static-depth typed_pool no-exceptions mode'
        expected = '[spsc::typed_pool]: no-exceptions mode requires object allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_STATIC_TYPED_POOL=1')
    },
    @{
        name = 'throwing slot allocator in dynamic typed_pool no-exceptions mode'
        expected = '[spsc::typed_pool]: no-exceptions mode requires slot allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_DYNAMIC_TYPED_POOL=1')
    },
    @{
        name = 'throwing allocator in raw latest no-exceptions mode'
        expected = '[spsc::latest<void,0>]: no-exceptions mode requires slot allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_RAW_LATEST=1')
    },
    @{
        name = 'throwing allocator in typed latest no-exceptions mode'
        expected = '[spsc::latest<T,0>]: no-exceptions mode requires allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_TYPED_LATEST=1')
    },
    @{
        name = 'throwing allocator in dynamic-count buffer_pool no-exceptions mode'
        expected = '[spsc::buffer_pool]: no-exceptions mode requires allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_BUFFER_COUNT=1')
    },
    @{
        name = 'throwing allocator in dynamic-size buffer_pool no-exceptions mode'
        expected = '[spsc::buffer_pool]: no-exceptions mode requires byte allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_BUFFER_SIZE=1')
    },
    @{
        name = 'throwing allocator in fully dynamic buffer_pool no-exceptions mode'
        expected = '[spsc::buffer_pool]: no-exceptions mode requires slot allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_ALLOCATOR_BUFFER_SHAPE=1')
    },
    @{
        name = 'throwing transient table allocator in static-depth pool no-exceptions mode'
        expected = '[spsc::pool]: no-exceptions mode requires slot allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_POINTER_TABLE_STATIC_POOL=1')
    },
    @{
        name = 'throwing transient table allocator in static typed_pool no-exceptions mode'
        expected = '[spsc::typed_pool]: no-exceptions mode requires slot allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_POINTER_TABLE_STATIC_TYPED_POOL=1')
    },
    @{
        name = 'throwing transient table allocator in fixed-count buffer_pool no-exceptions mode'
        expected = '[spsc::buffer_pool]: no-exceptions mode requires pointer-table allocator::allocate(size_type) to be noexcept.'
        defines = @('SPSC_TEST_THROWING_POINTER_TABLE_FIXED_COUNT_BUFFER_POOL=1')
    },
    @{
        name = 'throwing value destructor'
        expected = '[spsc::queue]: value_type destructor must be noexcept.'
        defines = @('SPSC_TEST_THROWING_DESTRUCTOR=1')
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
            $source
        )
        $arguments += @($scenario.defines | ForEach-Object { "/D$_" })
    } else {
        $arguments = @(
            '-std=c++17',
            "-I$repoRoot",
            "-I$includeDir",
            '-fsyntax-only',
            $source
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

Write-Output "PASS: invalid container extension contracts were rejected by $compilerPath"
exit 0
