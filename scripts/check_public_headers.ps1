[CmdletBinding()]
param(
    [string]$Compiler = 'g++'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$srcRoot = Join-Path $repoRoot 'src'
$spscRoot = Join-Path $srcRoot 'spsc'
$compilerPath = (Get-Command -Name $Compiler -ErrorAction Stop).Source
$isMsvc = [IO.Path]::GetFileName($compilerPath).Equals(
    'cl.exe',
    [StringComparison]::OrdinalIgnoreCase)

$headers = @(
    Get-Item -LiteralPath (Join-Path $repoRoot 'basic_types.h')
    Get-ChildItem -LiteralPath $spscRoot -Recurse -File -Filter '*.hpp'
) | Sort-Object -Property FullName

foreach ($header in $headers) {
    if ($isMsvc) {
        $arguments = @(
            '/nologo',
            '/std:c++17',
            '/permissive-',
            '/W4',
            '/WX',
            '/EHsc',
            "/I$repoRoot",
            "/I$srcRoot",
            "/I$spscRoot",
            '/TP',
            '/Zs',
            $header.FullName
        )
    } else {
        $arguments = @(
            '-std=c++17',
            '-Wall',
            '-Wextra',
            '-Werror',
            '-pedantic-errors',
            "-I$repoRoot",
            "-I$srcRoot",
            "-I$spscRoot",
            '-x',
            'c++',
            '-fsyntax-only',
            $header.FullName
        )
    }

    & $compilerPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Public header is not self-contained: $($header.FullName)"
    }
}

Write-Output "PASS: $($headers.Count) public headers compile independently with $compilerPath"
