# Copyright (c) Microsoft Corporation. All rights reserved.
#
# Run-AsaInstallDiff.ps1
#
# Attack Surface Analyzer (ASA) install-diff for the WSL MSI, intended to run on a
# clean Windows agent (CI nightly) but also usable locally. It:
#   1. (optionally) uninstalls any pre-existing WSL MSI to get a clean baseline,
#   2. collects a baseline snapshot,
#   3. installs the supplied wsl.msi,
#   4. collects a second snapshot,
#   5. exports the diff as SARIF + JSON,
#   6. filters ASA analyzer findings through tools/devops/asa-expected-findings.json,
#   7. writes a net-new findings report and (optionally) fails if any remain.
#
# This satisfies the Continuous SDL requirement that installers / high-privilege
# programs do not weaken the OS security configuration.

[CmdletBinding()]
param(
    # Path to the wsl.msi to install and analyze.
    [Parameter(Mandatory = $true)]
    [string] $MsiPath,

    # Working directory for the ASA database and exported reports.
    [string] $WorkDir = (Join-Path $env:TEMP 'wsl-asa'),

    # Allowlist of known-benign findings.
    [string] $ExpectedFindingsPath = (Join-Path $PSScriptRoot 'asa-expected-findings.json'),

    # Directory the installer deploys to (scopes the file-system collector).
    [string] $InstallDir = 'C:\Program Files\WSL',

    # Treat all unsigned WSL PE binaries under $InstallDir as expected. Use for
    # unsigned dev/nightly builds where ESRP signing has not run. Accepts 1/0 or
    # true/false (string, so it binds correctly when passed via powershell.exe -File).
    [string] $AllowUnsignedWslBinaries = '0',

    # Exit non-zero when net-new (non-allowlisted) findings remain. Off by default
    # so the stage can be introduced as non-gating, then flipped on once clean.
    # Accepts 1/0 or true/false (string, for the same -File binding reason).
    [string] $FailOnNewFindings = '0',

    # When set, write a minimal TRX result file here so CloudTest's TRX parser can
    # surface the ASA job as a single pass/fail test. Empty = skip (local runs).
    [string] $TrxPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The switches arrive as strings (powershell.exe -File coerces all args to string,
# and [switch]/[bool] reject "1"/"0" there). Normalize to real booleans here.
function ConvertTo-AsaBool([string] $value) {
    return ($value -eq '1' -or $value -ieq 'true' -or $value -ieq '$true')
}
$allowUnsignedWslBinaries = ConvertTo-AsaBool $AllowUnsignedWslBinaries
$failOnNewFindings = ConvertTo-AsaBool $FailOnNewFindings

# Emit a minimal, well-formed TRX so CloudTest (Parser="TRX") reports the ASA job
# as one pass/fail test. Job-level pass/fail is still driven by the process exit
# code; this just gives a readable result row + stdout in the ADO test tab.
function Write-AsaTrx {
    param(
        [ValidateSet('Passed', 'Failed')] [string] $Outcome,
        [string] $Message = ''
    )
    if ([string]::IsNullOrEmpty($TrxPath)) { return }
    try {
        $dir = Split-Path -Parent $TrxPath
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
        $now = (Get-Date).ToString('o')
        $comp = if ($env:COMPUTERNAME) { $env:COMPUTERNAME } else { 'cloudtest' }
        $g = { [guid]::NewGuid().ToString() }
        $testId = & $g; $execId = & $g; $listId = & $g
        $passed = if ($Outcome -eq 'Passed') { 1 } else { 0 }
        $failed = if ($Outcome -eq 'Failed') { 1 } else { 0 }
        $esc = [System.Security.SecurityElement]::Escape([string]$Message)
        $xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<TestRun id="$(& $g)" name="ASA install-diff" xmlns="http://microsoft.com/schemas/VisualStudio/TeamTest/2010">
  <Times creation="$now" queuing="$now" start="$now" finish="$now" />
  <ResultSummary outcome="Completed">
    <Counters total="1" executed="1" passed="$passed" failed="$failed" error="0" timeout="0" aborted="0" inconclusive="0" passedButRunAborted="0" notRunnable="0" notExecuted="0" disconnected="0" warning="0" completed="0" inProgress="0" pending="0" />
  </ResultSummary>
  <TestDefinitions>
    <UnitTest name="Asa.InstallDiff" storage="run-asainstalldiff.ps1" id="$testId">
      <Execution id="$execId" />
      <TestMethod codeBase="Run-AsaInstallDiff.ps1" adapterTypeName="executor://mstestadapter/v2" className="Asa" name="Asa.InstallDiff" />
    </UnitTest>
  </TestDefinitions>
  <TestEntries>
    <TestEntry testId="$testId" executionId="$execId" testListId="$listId" />
  </TestEntries>
  <TestLists>
    <TestList name="Results Not in a List" id="$listId" />
    <TestList name="All Loaded Results" id="$(& $g)" />
  </TestLists>
  <Results>
    <UnitTestResult executionId="$execId" testId="$testId" testName="Asa.InstallDiff" computerName="$comp" duration="00:00:00" startTime="$now" endTime="$now" testType="13cdc9d9-ddb5-4fa4-a97d-d965ccfc6d4b" outcome="$Outcome" testListId="$listId">
      <Output><StdOut>$esc</StdOut></Output>
    </UnitTestResult>
  </Results>
</TestRun>
"@
        Set-Content -Path $TrxPath -Value $xml -Encoding UTF8
        Write-Host "TRX written: $TrxPath ($Outcome)"
    }
    catch {
        Write-Host "WARNING: failed to write TRX: $_"
    }
}

# Any uncaught terminating error (failed collect/install/export, gate failure,
# missing prerequisites) lands here: record a failed TRX and exit non-zero.
trap {
    Write-Host "ERROR: $_"
    Write-AsaTrx -Outcome 'Failed' -Message ("{0}`n{1}" -f $_, $_.ScriptStackTrace)
    exit 1
}

if (-not (Test-Path $MsiPath)) { throw "MSI not found: $MsiPath" }
$MsiPath = (Resolve-Path $MsiPath).Path
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$db          = Join-Path $WorkDir 'asa.sqlite'
$installLog  = Join-Path $WorkDir 'wsl-msi-install.log'
$sarifPath   = Join-Path $WorkDir 'wsl_clean_vs_wsl_after_summary.Sarif'
$reportPath  = Join-Path $WorkDir 'asa-net-new-findings.json'
$collectors  = @('-c', '-C', '-d', '-F', '-p', '-r', '-s', '-u', '-f', '--directories', $InstallDir)

function Test-Admin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    throw 'Run-AsaInstallDiff.ps1 must run elevated (ASA needs admin to read ACLs and install the MSI).'
}

# --- Ensure the ASA CLI is available ---
# ASA ships a prebuilt Windows CLI zip (ASA_win), the last such build being 2.3.321
# (newer tags ship the dotnet global tool only). That zip is framework-dependent on
# the .NET 9 runtimes (Microsoft.NETCore.App + Microsoft.AspNetCore.App), so on a
# clean VM image we also install the matching ASP.NET Core runtime before running it.
$AsaVersion = '2.3.321'
$AsaUrl = "https://github.com/microsoft/AttackSurfaceAnalyzer/releases/download/v$AsaVersion/ASA_win_$AsaVersion.zip"
$AsaDotnetChannel = '9.0'
$DotnetRoot = 'C:\Program Files\dotnet'

function Install-AsaDotnetRuntime {
    # ASA_win is a framework-dependent .NET 9 app. Install the ASP.NET Core 9 shared
    # runtime (which carries the base .NET runtime too) into the default location so
    # the apphost resolves it; also pin DOTNET_ROOT for deterministic resolution.
    Write-Host "=== Installing .NET $AsaDotnetChannel runtime for ASA ===" -ForegroundColor Cyan
    $installer = Join-Path $WorkDir 'dotnet-install.ps1'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri 'https://dot.net/v1/dotnet-install.ps1' -OutFile $installer -UseBasicParsing
    & $installer -Channel $AsaDotnetChannel -Runtime aspnetcore -InstallDir $DotnetRoot
    $sharedRoot = Join-Path $DotnetRoot 'shared'
    $netCore = Join-Path $sharedRoot 'Microsoft.NETCore.App'
    $aspNet = Join-Path $sharedRoot 'Microsoft.AspNetCore.App'
    if (-not (Test-Path $netCore) -or -not (Test-Path $aspNet)) {
        throw "dotnet runtime bootstrap failed: expected shared frameworks under $sharedRoot"
    }
    $env:DOTNET_ROOT = $DotnetRoot
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:DOTNET_NOLOGO = '1'
}

function Resolve-Asa {
    $toolsDir = Join-Path $WorkDir 'asa-cli'
    $exe = Join-Path $toolsDir "ASA_win_$AsaVersion\Asa.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "=== Downloading Attack Surface Analyzer CLI $AsaVersion ===" -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null
        $zip = Join-Path $toolsDir "ASA_win_$AsaVersion.zip"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $AsaUrl -OutFile $zip -UseBasicParsing
        Expand-Archive -Path $zip -DestinationPath $toolsDir -Force
    }
    if (-not (Test-Path $exe)) { throw "ASA CLI not found after download (expected $exe)." }

    Install-AsaDotnetRuntime
    return $exe
}

$asa = Resolve-Asa
Write-Host "Using ASA: $asa"

function Get-WslMsiProductCodes {
    $codes = @()
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($r in $roots) {
        Get-ItemProperty $r -ErrorAction SilentlyContinue | Where-Object {
            $names = $_.PSObject.Properties.Name
            ($names -contains 'DisplayName') -and ($_.DisplayName -match 'Windows Subsystem for Linux') -and
            ($names -contains 'WindowsInstaller') -and ($_.WindowsInstaller -eq 1)
        } | ForEach-Object { $codes += $_.PSChildName }
    }
    $codes | Select-Object -Unique
}

# --- [0] Clean baseline: remove any pre-existing WSL MSI ---
Write-Host '=== [0/5] Ensuring clean baseline ===' -ForegroundColor Cyan
if (Get-Command wsl.exe -ErrorAction SilentlyContinue) {
    try { & wsl.exe --shutdown } catch { }
}
foreach ($code in (Get-WslMsiProductCodes)) {
    Write-Host "Uninstalling existing WSL product $code"
    $u = Start-Process msiexec.exe -ArgumentList @('/x', $code, '/qn', '/norestart') -Wait -PassThru
    Write-Host "  msiexec /x exit: $($u.ExitCode)"
}
Start-Sleep -Seconds 5

# --- [1] Baseline collect ---
Write-Host '=== [1/5] Baseline collect (wsl_clean) ===' -ForegroundColor Cyan
& $asa collect --runid wsl_clean @collectors --databasefilename $db --overwrite
if ($LASTEXITCODE -ne 0) { throw "Baseline collect failed ($LASTEXITCODE)" }

# --- [2] Install ---
Write-Host "=== [2/5] Installing $MsiPath ===" -ForegroundColor Cyan
$p = Start-Process msiexec.exe -ArgumentList @('/i', "`"$MsiPath`"", '/qn', '/norestart', '/l*v', "`"$installLog`"") -Wait -PassThru
Write-Host "msiexec /i exit code: $($p.ExitCode)"
if ($p.ExitCode -notin @(0, 3010, 1641)) {
    throw "MSI install failed ($($p.ExitCode)); see $installLog"
}

# --- [3] Second collect ---
Write-Host '=== [3/5] Second collect (wsl_after) ===' -ForegroundColor Cyan
& $asa collect --runid wsl_after @collectors --databasefilename $db --overwrite
if ($LASTEXITCODE -ne 0) { throw "Second collect failed ($LASTEXITCODE)" }

# --- [4] Export diff ---
Write-Host '=== [4/5] Export diff (SARIF + JSON) ===' -ForegroundColor Cyan
& $asa export-collect --firstrunid wsl_clean --secondrunid wsl_after --databasefilename $db --outputpath $WorkDir
& $asa export-collect --firstrunid wsl_clean --secondrunid wsl_after --databasefilename $db --outputpath $WorkDir --outputsarif
if (-not (Test-Path $sarifPath)) { throw "Expected SARIF not produced at $sarifPath" }

# --- [5] Filter findings through the allowlist ---
Write-Host '=== [5/5] Triage findings against allowlist ===' -ForegroundColor Cyan
$expected = Get-Content $ExpectedFindingsPath -Raw | ConvertFrom-Json
$sarif    = Get-Content $sarifPath -Raw | ConvertFrom-Json -AsHashtable
$results  = $sarif.runs[0].results

function Convert-GlobToRegex([string] $glob) {
    $escaped = [Regex]::Escape($glob) -replace '\\\*', '.*' -replace '\\\?', '.'
    return '^' + $escaped + '$'
}

function Get-FindingPath([string] $text) {
    # Messages look like "Missing ASLR: C:\path\file (CREATED)".
    if ($text -match ':\s*(.+?)\s*\([A-Z]+\)\s*$') { return $Matches[1].Trim() }
    $idx = $text.IndexOf(':')
    if ($idx -ge 0) { return $text.Substring($idx + 1).Trim() }
    return $text.Trim()
}

function Test-Expected([string] $ruleId, [string] $path) {
    if ($expected.ignoreRules -contains $ruleId) { return $true }
    $norm = $path.Replace('\', '/')
    foreach ($entry in $expected.expected) {
        if ($entry.ruleId -ne $ruleId) { continue }
        foreach ($g in $entry.pathGlobs) {
            $rx = Convert-GlobToRegex ($g.Replace('\', '/'))
            if ($norm -match $rx) { return $true }
        }
    }
    if ($allowUnsignedWslBinaries -and $ruleId -eq 'Unsigned binaries') {
        $instNorm = $InstallDir.Replace('\', '/')
        if ($norm.StartsWith($instNorm, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

$netNew = @()
$allowed = @()
foreach ($r in $results) {
    $ruleId = if ($r.ContainsKey('ruleId')) { [string]$r.ruleId } else { 'Default Level' }
    $text   = if ($r.ContainsKey('message') -and $r.message.ContainsKey('text')) { [string]$r.message.text } else { '' }
    $path   = Get-FindingPath $text
    $record = [ordered]@{ ruleId = $ruleId; path = $path; message = $text }
    if (Test-Expected $ruleId $path) { $allowed += $record } else { $netNew += $record }
}

$netNew = $netNew | Sort-Object { $_.ruleId }, { $_.path } -Unique

$summary = [ordered]@{
    msi              = $MsiPath
    totalResults     = $results.Count
    allowlistedCount = $allowed.Count
    netNewCount      = $netNew.Count
    netNew           = $netNew
}
$summary | ConvertTo-Json -Depth 6 | Set-Content $reportPath -Encoding UTF8

Write-Host ''
Write-Host "ASA results: $($results.Count) total, $($allowed.Count) allowlisted, $($netNew.Count) net-new." -ForegroundColor Green
Write-Host "SARIF:  $sarifPath"
Write-Host "Report: $reportPath"

if ($netNew.Count -gt 0) {
    Write-Host ''
    Write-Warning "$($netNew.Count) net-new ASA finding(s) not covered by the allowlist:"
    $netNew | Group-Object { $_.ruleId } | ForEach-Object {
        Write-Host ("  [{0}] x{1}" -f $_.Name, $_.Count) -ForegroundColor Yellow
        $_.Group | Select-Object -First 25 | ForEach-Object { Write-Host "      $($_.path)" }
    }
    Write-Host ''
    Write-Host 'Review each finding. If benign, add it to tools/devops/asa-expected-findings.json with a rationale; otherwise fix the installer/code.'
    if ($failOnNewFindings) {
        throw "ASA install-diff found $($netNew.Count) net-new finding(s)."
    }
}
else {
    Write-Host 'No net-new attack-surface findings. Install-diff is clean against the allowlist.' -ForegroundColor Green
}

# Reached only on success (clean, or net-new while non-gating): report a passing TRX.
Write-AsaTrx -Outcome 'Passed' -Message ("ASA install-diff: {0} total finding(s), {1} allowlisted, {2} net-new." -f $results.Count, $allowed.Count, $netNew.Count)
exit 0
