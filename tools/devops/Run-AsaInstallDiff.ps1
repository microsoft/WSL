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
    # unsigned dev/nightly builds where ESRP signing has not run.
    [switch] $AllowUnsignedWslBinaries,

    # Exit non-zero when net-new (non-allowlisted) findings remain. Off by default
    # so the stage can be introduced as non-gating, then flipped on once clean.
    [switch] $FailOnNewFindings
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

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
$asa = Get-Command asa -ErrorAction SilentlyContinue
if (-not $asa) {
    $toolPath = Join-Path $env:USERPROFILE '.dotnet\tools\asa.exe'
    if (-not (Test-Path $toolPath)) {
        Write-Host '=== Installing Attack Surface Analyzer CLI ===' -ForegroundColor Cyan
        & dotnet tool install --global Microsoft.CST.AttackSurfaceAnalyzer.CLI
        if ($LASTEXITCODE -ne 0) { throw "Failed to install ASA CLI ($LASTEXITCODE)" }
    }
    $asa = $toolPath
}
else {
    $asa = $asa.Source
}
Write-Host "Using ASA: $asa"

function Get-WslMsiProductCodes {
    $codes = @()
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($r in $roots) {
        Get-ItemProperty $r -ErrorAction SilentlyContinue | Where-Object {
            $_.DisplayName -match 'Windows Subsystem for Linux' -and $_.WindowsInstaller -eq 1
        } | ForEach-Object { $codes += $_.PSChildName }
    }
    $codes | Select-Object -Unique
}

# --- [0] Clean baseline: remove any pre-existing WSL MSI ---
Write-Host '=== [0/5] Ensuring clean baseline ===' -ForegroundColor Cyan
& wsl.exe --shutdown 2>$null
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
    if ($AllowUnsignedWslBinaries -and $ruleId -eq 'Unsigned binaries') {
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
    if ($FailOnNewFindings) {
        throw "ASA install-diff found $($netNew.Count) net-new finding(s)."
    }
}
else {
    Write-Host 'No net-new attack-surface findings. Install-diff is clean against the allowlist.' -ForegroundColor Green
}
