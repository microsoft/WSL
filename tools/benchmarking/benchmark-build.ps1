<#
.SYNOPSIS
    Benchmarks full and incremental build times for a CMake target.

.DESCRIPTION
    Runs N full builds (object directory wiped before each) and M incremental builds
    (a randomly chosen .cpp under -SourceDir gets a comment appended, then the target
    is rebuilt and the file is restored byte-for-byte).

    After each incremental iteration the restored file is rebuilt in an untimed settle
    build, so a measurement only ever covers the file touched by that iteration.

    Results are written to a CSV and a summary is printed.

.PARAMETER Target
    CMake target to build. Defaults to 'wsltests'.

.PARAMETER TargetDir
    Directory (relative to the repo root, or absolute) holding the CMakeLists.txt that
    defines -Target. Used to locate the target's object directory. Defaults to
    'test\windows'.

.PARAMETER SourceDir
    Directory whose .cpp files are touched during incremental builds. Defaults to
    -TargetDir.

.PARAMETER ExcludeDirName
    Directory names skipped when collecting incremental candidates.

.EXAMPLE
    powershell tools\benchmarking\benchmark-build.ps1 -Label before -FullBuilds 10 -IncrementalBuilds 100

.EXAMPLE
    powershell tools\benchmarking\benchmark-build.ps1 -Label wslc-only -SourceDir test\windows\wslc

.EXAMPLE
    powershell tools\benchmarking\benchmark-build.ps1 -Label svc -Target wslservice -TargetDir src\windows\service\exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Label,
    [int]$FullBuilds = 10,
    [int]$IncrementalBuilds = 100,
    [string]$Config = 'debug',
    [string]$Target = 'wsltests',
    [string]$TargetDir = 'test\windows',
    [string]$SourceDir,
    [string[]]$ExcludeDirName = @('testplugin'),
    [string]$OutDir = 'tools\benchmarking\benchmark-results',
    [int]$Seed = 20260803
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repoRoot
try
{
    function Resolve-RepoPath
    {
        param([string]$Path)

        if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
        return (Join-Path $repoRoot $Path)
    }

    if (-not $SourceDir) { $SourceDir = $TargetDir }

    $targetDirFull = Resolve-RepoPath $TargetDir
    $sourceDirFull = Resolve-RepoPath $SourceDir
    $objDir = Join-Path $targetDirFull "$Target.dir\$Config"

    if (-not (Test-Path (Join-Path $repoRoot 'CMakeCache.txt')))
    {
        throw "CMakeCache.txt not found in $repoRoot. Run 'cmake .' first."
    }

    if (-not (Test-Path $targetDirFull))
    {
        throw "TargetDir not found: $targetDirFull"
    }

    if (-not (Test-Path $sourceDirFull))
    {
        throw "SourceDir not found: $sourceDirFull"
    }

    if (-not (Test-Path $OutDir))
    {
        New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    }

    $csvPath = Join-Path $OutDir "$Label.csv"
    $logPath = Join-Path $OutDir "$Label.log"
    Remove-Item $csvPath, $logPath -ErrorAction SilentlyContinue

    $script:results = New-Object System.Collections.Generic.List[object]

    function Invoke-TimedBuild
    {
        param([string]$Phase, [int]$Iteration, [int]$Total, [string]$TouchedFile)

        $sw = [Diagnostics.Stopwatch]::StartNew()
        $output = & cmake --build . --target $Target --config $Config -- -m 2>&1
        $sw.Stop()
        $exit = $LASTEXITCODE

        Add-Content -Path $logPath -Value "===== $Phase #$Iteration ($TouchedFile) exit=$exit elapsed=$($sw.Elapsed.TotalSeconds) ====="
        Add-Content -Path $logPath -Value ($output | Out-String)

        if ($exit -ne 0)
        {
            throw "Build failed during $Phase iteration $Iteration. See $logPath"
        }

        $record = [pscustomobject]@{
            Label     = $Label
            Phase     = $Phase
            Iteration = $Iteration
            File      = $TouchedFile
            Seconds   = [math]::Round($sw.Elapsed.TotalSeconds, 3)
        }
        $script:results.Add($record)
        $record | Export-Csv -Path $csvPath -NoTypeInformation -Append
        Write-Host ("[{0}] {1,-11} {2,3}/{3,-3} {4,7:N2}s  {5}" -f $Label, $Phase, $Iteration, $Total, $record.Seconds, $TouchedFile)
    }

    Write-Host "Warm-up build (bring all dependencies up to date)..." -ForegroundColor Cyan

    function Reset-DebugDatabase
    {
        # Incremental links keep appending to the program database, which eventually
        # trips LNK1140 (4 GB limit) part way through a long run.
        Get-ChildItem (Join-Path $repoRoot 'bin') -Recurse -Include "$Target.pdb", "$Target.ilk" -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }

    Reset-DebugDatabase
    & cmake --build . --target $Target --config $Config -- -m | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Warm-up build failed.' }

    Write-Host "`n=== FULL BUILDS ($FullBuilds) ===" -ForegroundColor Cyan
    for ($i = 1; $i -le $FullBuilds; $i++)
    {
        if (Test-Path $objDir) { Remove-Item $objDir -Recurse -Force }
        Reset-DebugDatabase
        Invoke-TimedBuild -Phase 'full' -Iteration $i -Total $FullBuilds -TouchedFile '(clean)'
    }

    Write-Host "`n=== INCREMENTAL BUILDS ($IncrementalBuilds) ===" -ForegroundColor Cyan
    $candidates = @(
        Get-ChildItem -Path $sourceDirFull -Filter *.cpp -Recurse |
            Where-Object { (Split-Path $_.DirectoryName -Leaf) -notin $ExcludeDirName } |
            Select-Object -ExpandProperty FullName
    )
    if ($candidates.Count -eq 0) { throw "No .cpp files found under $sourceDirFull" }
    Write-Host "Incremental candidate pool: $($candidates.Count) file(s) from $sourceDirFull"

    # Touches accumulate for the duration of the run and every file is restored at the
    # end. Restoring in the loop would re-dirty the file and make the next iteration
    # rebuild this iteration's translation unit as well as its own.
    $rand = New-Object System.Random($Seed)
    $originals = @{}
    try
    {
        for ($i = 1; $i -le $IncrementalBuilds; $i++)
        {
            $file = $candidates[$rand.Next(0, $candidates.Count)]
            if (-not $originals.ContainsKey($file))
            {
                $originals[$file] = [System.IO.File]::ReadAllBytes($file)
            }

            Add-Content -Path $file -Value "`n// build-benchmark touch $Label $i"
            Invoke-TimedBuild -Phase 'incremental' -Iteration $i -Total $IncrementalBuilds -TouchedFile (Split-Path $file -Leaf)
        }
    }
    finally
    {
        foreach ($entry in $originals.GetEnumerator())
        {
            [System.IO.File]::WriteAllBytes($entry.Key, $entry.Value)
        }
        Write-Host "Restored $($originals.Count) touched file(s)."
    }

    Write-Host "`n=== SUMMARY ($Label) ===" -ForegroundColor Green
    $script:results | Group-Object Phase | ForEach-Object {
        $s = $_.Group.Seconds | Measure-Object -Average -Minimum -Maximum -Sum
        $sorted = @($_.Group.Seconds | Sort-Object)
        $median = if ($sorted.Count % 2) { $sorted[[int]($sorted.Count / 2)] } else { ($sorted[$sorted.Count / 2 - 1] + $sorted[$sorted.Count / 2]) / 2 }
        [pscustomobject]@{
            Phase   = $_.Name
            Count   = $s.Count
            MeanSec = [math]::Round($s.Average, 2)
            MedSec  = [math]::Round($median, 2)
            MinSec  = [math]::Round($s.Minimum, 2)
            MaxSec  = [math]::Round($s.Maximum, 2)
            TotSec  = [math]::Round($s.Sum, 2)
        }
    } | Format-Table -AutoSize

    Write-Host "Results: $csvPath"
}
finally
{
    Pop-Location
}
