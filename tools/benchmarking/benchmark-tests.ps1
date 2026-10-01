#Requires -RunAsAdministrator

<#
.SYNOPSIS
    Runs the WSL test suite repeatedly with a given filter and reports timing statistics.

.DESCRIPTION
    Wraps bin\<arch>\<config>\test.bat in a timing loop so a change's effect on test runtime can
    be measured rather than estimated. Each pass is timed individually, its log is kept, and the
    results are written incrementally to a CSV so a run in progress can be monitored.

    Building and branch selection are the caller's responsibility: this script measures whatever
    is currently in the bin directory. To compare two revisions, build one, run this, rebuild the
    other, and run it again with a different -Label.

    Runtime is dominated by per-invocation setup unless -f is included in the filter, which skips
    package and distro installation (requires "wsl --set-default test_distro" to have been run).

.PARAMETER Filter
    Arguments passed verbatim to test.bat, typically a TAEF selection switch.

    They are written into a temporary .cmd rather than forwarded as PowerShell arguments, because
    test.bat re-invokes powershell.exe with %*, which would otherwise re-split them. Quotes that
    need to survive that hop must be backslash-escaped by the caller:

        /name:WSLCE2ETests::WSLCE2EPushPullTests::WSLCE2E_Image_PushPull
        /select:\"@TestCategory='WSLC'\"

    Note that TAEF honours only the first /name switch, so several cannot be OR'd together.

.PARAMETER Runs
    Number of passes to execute.

.PARAMETER Label
    Short name used for the CSV and per-run log file names. Defaults to "benchmark".

.PARAMETER Warmup
    Performs one untimed run before the measured passes and discards its result. The first run of
    a series is routinely several seconds slower than the rest because caches, the WSL session and
    the test distro are all cold, which skews the mean and inflates the standard deviation.

.PARAMETER OutputPath
    Directory for the CSV and logs. Defaults to a wsl-test-benchmark folder under %TEMP%.

.PARAMETER Arch
    Build architecture to locate test.bat under bin. Defaults to x64.

.PARAMETER Config
    Build configuration to locate test.bat under bin. Defaults to debug.

.EXAMPLE
    .\benchmark-tests.ps1 -Runs 10 -Warmup -Filter '/name:WSLCE2ETests::WSLCE2EPushPullTests::WSLCE2E_Image_PushPull -f'

    Times one test over ten passes in fast mode, discarding a cold first run.

.EXAMPLE
    .\benchmark-tests.ps1 -Runs 3 -Filter '/select:\"@TestCategory=''WSLC''\"' -Label wslc-suite

    Times the whole WSLC category three times.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Filter,
    [ValidateRange(1, 1000)][int]$Runs = 1,
    [string]$Label = 'benchmark',
    [switch]$Warmup,
    [string]$OutputPath = (Join-Path $env:TEMP 'wsl-test-benchmark'),
    [ValidateSet('x64', 'arm64')][string]$Arch = 'x64',
    [ValidateSet('debug', 'release')][string]$Config = 'debug'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$testBat = Join-Path $repoRoot "bin\$Arch\$Config\test.bat"
if (-not (Test-Path $testBat))
{
    throw "test.bat not found at $testBat. Build the project first."
}

# TAEF logs interleave UTF-16 and UTF-8, so decoding as either alone silently loses most lines.
# Dropping NUL bytes yields readable text for both. Shared access allows reading a live log.
function Read-TestLog([string]$Path)
{
    if (-not (Test-Path $Path))
    {
        return @()
    }

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try
    {
        $buffer = New-Object IO.MemoryStream
        $stream.CopyTo($buffer)
    }
    finally
    {
        $stream.Close()
    }

    $characters = foreach ($byte in $buffer.ToArray())
    {
        if ($byte -ne 0)
        {
            [char]$byte
        }
    }

    return (($characters -join '') -split "`r?`n")
}

function Get-Statistics([double[]]$Values)
{
    $sorted = @($Values | Sort-Object)
    $count = $sorted.Count
    $mean = ($sorted | Measure-Object -Average).Average

    $median = if ($count % 2)
    {
        $sorted[[int][math]::Floor($count / 2)]
    }
    else
    {
        ($sorted[$count / 2 - 1] + $sorted[$count / 2]) / 2
    }

    $standardDeviation = 0.0
    if ($count -gt 1)
    {
        $sumOfSquares = ($sorted | ForEach-Object { [math]::Pow($_ - $mean, 2) } | Measure-Object -Sum).Sum
        $standardDeviation = [math]::Sqrt($sumOfSquares / ($count - 1))
    }

    return [pscustomobject]@{
        Runs              = $count
        Min               = $sorted[0]
        Median            = $median
        Mean              = $mean
        Max               = $sorted[-1]
        StandardDeviation = $standardDeviation
    }
}

New-Item -ItemType Directory -Force -Path $OutputPath | Out-Null
$csvPath = Join-Path $OutputPath "$Label.csv"
$wrapperPath = Join-Path $OutputPath "$Label-wrapper.cmd"

@"
@echo off
call "$testBat" $Filter
exit /b %ERRORLEVEL%
"@ | Set-Content -Path $wrapperPath -Encoding ASCII

'Run,Seconds,Total,Passed,Failed,Skipped,ExitCode,Log' | Set-Content -Path $csvPath -Encoding ASCII

Write-Host "Filter : $Filter"
Write-Host "Runs   : $Runs$(if ($Warmup) { ' (plus one untimed warmup)' })"
Write-Host "Output : $OutputPath"
Write-Host ''

$durations = [System.Collections.Generic.List[double]]::new()
$failedRuns = 0

try
{
    if ($Warmup)
    {
        $warmupLog = Join-Path $OutputPath "$Label-warmup.log"
        Remove-Item $warmupLog -ErrorAction SilentlyContinue

        $stopwatch = [Diagnostics.Stopwatch]::StartNew()
        & cmd.exe /c "call `"$wrapperPath`" > `"$warmupLog`" 2>&1"
        $stopwatch.Stop()

        Write-Host ("warmup : {0,8:N2}s  (discarded)" -f $stopwatch.Elapsed.TotalSeconds)
    }

    for ($run = 1; $run -le $Runs; $run++)
    {
        $logPath = Join-Path $OutputPath "$Label-run$run.log"
        Remove-Item $logPath -ErrorAction SilentlyContinue

        $stopwatch = [Diagnostics.Stopwatch]::StartNew()
        & cmd.exe /c "call `"$wrapperPath`" > `"$logPath`" 2>&1"
        $exitCode = $LASTEXITCODE
        $stopwatch.Stop()

        $total = $passed = $failed = $skipped = ''
        $summary = Read-TestLog $logPath | Select-String -SimpleMatch 'Summary: Total=' | Select-Object -First 1
        if ($summary -and $summary.Line -match 'Total=(\d+).*?Passed=(\d+).*?Failed=(\d+).*?Skipped=(\d+)')
        {
            $total, $passed, $failed, $skipped = $Matches[1], $Matches[2], $Matches[3], $Matches[4]
            if ([int]$failed -gt 0)
            {
                $failedRuns++
            }
        }

        $seconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 2)
        $durations.Add($seconds)
        "$run,$seconds,$total,$passed,$failed,$skipped,$exitCode,$(Split-Path $logPath -Leaf)" |
            Add-Content -Path $csvPath -Encoding ASCII

        Write-Host ("run {0}/{1}: {2,8:N2}s  total={3} passed={4} failed={5} skipped={6}" -f `
                $run, $Runs, $seconds, $total, $passed, $failed, $skipped)
    }
}
finally
{
    Remove-Item $wrapperPath -ErrorAction SilentlyContinue
}

$statistics = Get-Statistics $durations.ToArray()

Write-Host ''
Write-Host "=== $Label ==="
Write-Host ("min {0:N2}s | median {1:N2}s | mean {2:N2}s | max {3:N2}s | sd {4:N2}s (n={5})" -f `
        $statistics.Min, $statistics.Median, $statistics.Mean, $statistics.Max, $statistics.StandardDeviation, $statistics.Runs)

if ($failedRuns -gt 0)
{
    Write-Warning "$failedRuns of $Runs run(s) reported test failures. Timings may not be comparable."
}

Write-Host "CSV: $csvPath"
