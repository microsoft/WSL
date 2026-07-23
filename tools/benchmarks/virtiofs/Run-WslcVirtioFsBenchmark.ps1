# Copyright (C) Microsoft Corporation. All rights reserved.

[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()]
    [int[]]$QueueCounts = @(1, 2, 4, 8),

    [ValidateNotNullOrEmpty()]
    [int[]]$ShareCounts = @(1, 2, 4, 8, 16),

    [ValidateSet("git-clone", "source-extract")]
    [string[]]$Workloads = @("git-clone", "source-extract"),

    [ValidateRange(1, 100)]
    [int]$Repetitions = 5,

    [ValidateRange(0, 10)]
    [int]$WarmupRuns = 1,

    [string]$Image = "wslc-virtiofs-benchmark:local",

    [string]$WslcPath,

    [string]$WorkDirectory = (Join-Path $env:TEMP "wslc-virtiofs-benchmark"),

    [string]$OutputDirectory = (Join-Path $PWD "virtiofs-results"),

    [switch]$SkipImageBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = (Resolve-Path (Join-Path $scriptDirectory "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($WslcPath))
{
    $developmentWslc = Join-Path $repositoryRoot "bin\x64\Debug\wslc.exe"
    $WslcPath = if (Test-Path $developmentWslc) { $developmentWslc } else { "wslc.exe" }
}

if ($QueueCounts | Where-Object { $_ -lt 1 -or $_ -gt 1024 })
{
    throw "QueueCounts must be between 1 and 1024."
}
if ($ShareCounts | Where-Object { $_ -lt 1 })
{
    throw "ShareCounts must be positive."
}

$settingsPath = Join-Path $env:LOCALAPPDATA "wslc\settings.yaml"
$settingsExisted = Test-Path $settingsPath
$settingsBackup = if ($settingsExisted) { [System.IO.File]::ReadAllBytes($settingsPath) } else { $null }
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$results = [System.Collections.Generic.List[object]]::new()

function Invoke-Wslc
{
    param(
        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [switch]$AllowFailure
    )

    Write-Verbose ("wslc " + ($Arguments -join " "))
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try
    {
        $output = @(& $WslcPath @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -ne 0 -and -not $AllowFailure)
    {
        throw "wslc exited with code ${exitCode}:`n$($output -join [Environment]::NewLine)"
    }

    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

function Stop-WslcSession
{
    $result = Invoke-Wslc -Arguments @("system", "session", "terminate") -AllowFailure
    if ($result.ExitCode -ne 0)
    {
        Write-Verbose "No running default WSLC session was terminated."
    }
}

function Set-BenchmarkSettings
{
    param([int]$QueueCount)

    $settingsDirectory = Split-Path -Parent $settingsPath
    [System.IO.Directory]::CreateDirectory($settingsDirectory) | Out-Null
    $content = @"
session:
  hostFileShareMode: virtiofs
experimental:
  virtioFsQueueCount: $QueueCount
"@
    [System.IO.File]::WriteAllText($settingsPath, $content, $utf8NoBom)
}

function Get-Percentile
{
    param(
        [double[]]$Values,
        [double]$Percentile
    )

    $ordered = @($Values | Sort-Object)
    $index = [Math]::Ceiling(($Percentile / 100) * $ordered.Count) - 1
    return $ordered[[Math]::Max(0, $index)]
}

function Invoke-BenchmarkRun
{
    param(
        [int]$QueueCount,
        [int]$ShareCount,
        [string]$Workload,
        [int]$Repetition,
        [bool]$Warmup
    )

    if (Test-Path $WorkDirectory)
    {
        Remove-Item -Recurse -Force $WorkDirectory
    }
    [System.IO.Directory]::CreateDirectory($WorkDirectory) | Out-Null

    $arguments = [System.Collections.Generic.List[string]]::new()
    $arguments.AddRange([string[]]@("container", "run", "--rm", "--name", "virtiofs-benchmark"))
    $guestPaths = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $ShareCount; $index++)
    {
        $hostPath = Join-Path $WorkDirectory "share-$index"
        [System.IO.Directory]::CreateDirectory($hostPath) | Out-Null
        $guestPath = "/bench/share-$index"
        $arguments.Add("--volume")
        $arguments.Add("${hostPath}:${guestPath}:rw")
        $guestPaths.Add($guestPath)
    }
    $arguments.Add($Image)
    $arguments.Add($Workload)
    $arguments.Add($QueueCount.ToString())
    $arguments.AddRange($guestPaths)

    $run = Invoke-Wslc -Arguments $arguments.ToArray() -AllowFailure
    $batchMs = $null
    $requestedQueueCount = $null
    $activeQueueCount = $null
    $msiVectorCount = $null
    $topology = @{}
    $workers = @{}
    foreach ($line in $run.Output)
    {
        $fields = $line -split "`t"
        if ($fields.Count -eq 4 -and $fields[0] -eq "TOPOLOGY")
        {
            $topology[[int]$fields[1]] = [pscustomobject]@{ Device = $fields[2]; Root = $fields[3] }
        }
        elseif ($fields.Count -eq 4 -and $fields[0] -eq "RESULT")
        {
            $workers[[int]$fields[1]] = [pscustomobject]@{ DurationMs = [long]$fields[2]; ExitCode = [int]$fields[3] }
        }
        elseif ($fields.Count -eq 4 -and $fields[0] -eq "QUEUES")
        {
            $requestedQueueCount = [int]$fields[1]
            $activeQueueCount = [int]$fields[2]
            $msiVectorCount = [int]$fields[3]
        }
        elseif ($fields.Count -eq 2 -and $fields[0] -eq "BATCH")
        {
            $batchMs = [long]$fields[1]
        }
    }

    if ($run.ExitCode -ne 0 -or $null -eq $batchMs -or $workers.Count -ne $ShareCount -or $topology.Count -ne $ShareCount -or
        $requestedQueueCount -ne $QueueCount -or $activeQueueCount -ne $QueueCount)
    {
        throw "Benchmark run failed or returned incomplete data:`n$($run.Output -join [Environment]::NewLine)"
    }

    foreach ($index in 0..($ShareCount - 1))
    {
        $results.Add([pscustomobject]@{
            Timestamp = [DateTime]::UtcNow.ToString("o")
            QueueCount = $QueueCount
            ActiveQueueCount = $activeQueueCount
            MsiVectorCount = $msiVectorCount
            ShareCount = $ShareCount
            Workload = $Workload
            Repetition = $Repetition
            Warmup = $Warmup
            BatchMs = $batchMs
            WorkerIndex = $index
            WorkerMs = $workers[$index].DurationMs
            ExitCode = $workers[$index].ExitCode
            Device = $topology[$index].Device
            FsRoot = $topology[$index].Root
        })
    }

    Write-Host ("queue={0} shares={1} workload={2} repetition={3} warmup={4} batch={5}ms" -f `
            $QueueCount, $ShareCount, $Workload, $Repetition, $Warmup, $batchMs)
}

try
{
    [System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    Set-BenchmarkSettings -QueueCount 1
    Stop-WslcSession

    if (-not $SkipImageBuild)
    {
        Write-Host "Building benchmark image $Image..."
        Invoke-Wslc -Arguments @("build", $scriptDirectory, "--file", (Join-Path $scriptDirectory "Dockerfile"), "--tag", $Image) | Out-Null
        Stop-WslcSession
    }

    foreach ($queueCount in $QueueCounts)
    {
        Set-BenchmarkSettings -QueueCount $queueCount
        Stop-WslcSession

        foreach ($shareCount in $ShareCounts)
        {
            foreach ($workload in $Workloads)
            {
                for ($warmup = 1; $warmup -le $WarmupRuns; $warmup++)
                {
                    Invoke-BenchmarkRun -QueueCount $queueCount -ShareCount $shareCount -Workload $workload -Repetition $warmup -Warmup $true
                }
                for ($repetition = 1; $repetition -le $Repetitions; $repetition++)
                {
                    Invoke-BenchmarkRun -QueueCount $queueCount -ShareCount $shareCount -Workload $workload -Repetition $repetition -Warmup $false
                }
            }
        }
    }

    $rawPath = Join-Path $OutputDirectory "raw.csv"
    $results | Export-Csv -NoTypeInformation -Encoding UTF8 $rawPath

    $summary = $results |
        Where-Object { -not $_.Warmup } |
        Group-Object QueueCount, ShareCount, Workload |
        ForEach-Object {
            $runs = @($_.Group | Group-Object Repetition | ForEach-Object { $_.Group[0] })
            $batchValues = [double[]]@($runs | ForEach-Object { $_.BatchMs })
            [pscustomobject]@{
                QueueCount = $runs[0].QueueCount
                ShareCount = $runs[0].ShareCount
                Workload = $runs[0].Workload
                Runs = $runs.Count
                MeanBatchMs = [Math]::Round(($batchValues | Measure-Object -Average).Average, 2)
                MedianBatchMs = Get-Percentile -Values $batchValues -Percentile 50
                P95BatchMs = Get-Percentile -Values $batchValues -Percentile 95
                MeanSharesPerSecond = [Math]::Round($runs[0].ShareCount / (($batchValues | Measure-Object -Average).Average / 1000), 3)
            }
        }

    $summaryPath = Join-Path $OutputDirectory "summary.csv"
    $summary | Sort-Object Workload, ShareCount, QueueCount | Export-Csv -NoTypeInformation -Encoding UTF8 $summaryPath
    Write-Host "Raw results: $rawPath"
    Write-Host "Summary: $summaryPath"
}
finally
{
    if ($settingsExisted)
    {
        [System.IO.File]::WriteAllBytes($settingsPath, $settingsBackup)
    }
    elseif (Test-Path $settingsPath)
    {
        Remove-Item -Force $settingsPath
    }

    Stop-WslcSession
}