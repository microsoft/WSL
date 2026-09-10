<#
.SYNOPSIS
    Runs benchmark-build.ps1 across several unity-build batch sizes.

.DESCRIPTION
    For each batch size the project is reconfigured, stale unity sources are removed,
    and a full benchmark run is performed. Labels are "<Prefix>-<name>", where a batch
    size of 0 is labelled "nounity".

.EXAMPLE
    powershell tools\benchmarking\run-unity-batch-sweep.ps1 -Prefix split-before

.EXAMPLE
    powershell tools\benchmarking\run-unity-batch-sweep.ps1 -Prefix wslc -SourceDir test\windows\wslc
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [int[]]$BatchSizes = @(0, 2, 4, 8),
    [int]$FullBuilds = 10,
    [int]$IncrementalBuilds = 100,
    [string]$Config = 'debug',
    [string]$Target = 'wsltests',
    [string]$TargetDir = 'test\windows',
    [string]$SourceDir,
    [string]$UnityVariable = 'WSL_UNITY_BATCH_SIZE'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repoRoot
try
{
    if (-not $SourceDir) { $SourceDir = $TargetDir }

    foreach ($batch in $BatchSizes)
    {
        $name = if ($batch -eq 0) { 'nounity' } else { "batch$batch" }
        $label = "$Prefix-$name"

        Write-Host "`n########## $label ($UnityVariable=$batch) ##########" -ForegroundColor Magenta

        & cmake . "-D$UnityVariable=$batch" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for batch size $batch." }

        # Generated unity sources are not pruned when the batch size changes, and the batch
        # size is shared by every unity-enabled target, so clear all of them.
        Get-ChildItem $repoRoot -Directory -Recurse -Filter Unity |
            Where-Object { $_.Parent.Name -like '*.dir' } |
            ForEach-Object { Remove-Item $_.FullName -Recurse -Force }

        & cmake . "-D$UnityVariable=$batch" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cmake reconfigure failed for batch size $batch." }

        & powershell -NoProfile -File (Join-Path $PSScriptRoot 'benchmark-build.ps1') `
            -Label $label -FullBuilds $FullBuilds -IncrementalBuilds $IncrementalBuilds `
            -Config $Config -Target $Target -TargetDir $TargetDir -SourceDir $SourceDir
        if ($LASTEXITCODE -ne 0) { throw "Benchmark failed for $label." }
    }

    Write-Host "`n########## SWEEP COMPLETE ($Prefix) ##########" -ForegroundColor Green
}
finally
{
    Pop-Location
}
