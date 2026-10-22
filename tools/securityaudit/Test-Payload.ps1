# Copyright (C) Microsoft Corporation. All rights reserved.

# Synthetic parser fixtures only. Real static linkage and BPF loading are separate Linux tests.
[CmdletBinding()]
param([string]$WorkDirectory)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PayloadHash.ps1')
if ([string]::IsNullOrEmpty($WorkDirectory))
{
    $WorkDirectory = Join-Path $PSScriptRoot '.payload-validator-test'
}
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$work = [IO.Path]::GetFullPath($WorkDirectory).TrimEnd('\', '/')
if (-not $work.StartsWith($sourceRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $work))
{
    throw 'Test work directory must be a new directory inside the WSL checkout.'
}
$null = New-Item -ItemType Directory -Path (Join-Path $work 'wsl-security-audit')
$validator = Join-Path $PSScriptRoot 'Validate-Payload.ps1'
$manifestPath = Join-Path $work 'audit-payload.json'
$collectorPath = Join-Path $work 'wsl-audit-collector'

function Set-U16([byte[]]$Bytes, [int]$Offset, [uint16]$Value) { [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset) }
function Set-U32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) { [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset) }
function Set-U64([byte[]]$Bytes, [int]$Offset, [uint64]$Value) { [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset) }

function New-Elf([uint16]$Machine, [switch]$Executable)
{
    $bytes = [byte[]]::new(128)
    $identity = [byte[]]@(0x7f, 0x45, 0x4c, 0x46, 2, 1, 1)
    $identity.CopyTo($bytes, 0)
    Set-U16 $bytes 16 $(if ($Executable) { 2 } else { 1 })
    Set-U16 $bytes 18 $Machine
    Set-U32 $bytes 20 1
    Set-U16 $bytes 52 64
    if ($Executable)
    {
        Set-U64 $bytes 24 0x400078
        Set-U64 $bytes 32 64
        Set-U16 $bytes 54 56
        Set-U16 $bytes 56 1
        Set-U32 $bytes 64 1
        Set-U32 $bytes 68 5
        Set-U64 $bytes 72 120
        Set-U64 $bytes 80 0x400078
        Set-U64 $bytes 96 8
        Set-U64 $bytes 104 8
    }
    return ,$bytes
}

function Write-Manifest
{
    foreach ($path in @($script:manifest.files.Keys))
    {
        $script:manifest.files[$path] = Get-PayloadFileSha256 (Join-Path $work $path.Replace('/', '\'))
    }
    [IO.File]::WriteAllText($manifestPath, ($script:manifest | ConvertTo-Json -Depth 10))
}

function Expect-Rejection([string]$Name, [scriptblock]$Action)
{
    $rejected = $false
    try { & $Action } catch { $rejected = $true }
    if (-not $rejected) { throw "Expected rejection: $Name" }
    Write-Host "PASS rejection: $Name"
}

function Read-Cpio([string]$Path)
{
    $bytes = [IO.File]::ReadAllBytes($Path)
    $offset = 0
    $entries = @{}
    while ($offset -lt $bytes.Length)
    {
        $header = [Text.Encoding]::ASCII.GetString($bytes, $offset, 110)
        if (-not $header.StartsWith('070701')) { throw 'Invalid CPIO header.' }
        $mode = [Convert]::ToInt32($header.Substring(14, 8), 16)
        $length = [Convert]::ToInt32($header.Substring(54, 8), 16)
        $nameLength = [Convert]::ToInt32($header.Substring(94, 8), 16)
        $name = [Text.Encoding]::ASCII.GetString($bytes, $offset + 110, $nameLength - 1)
        $offset = ($offset + 110 + $nameLength + 3) -band -4
        if ($name -eq 'TRAILER!!!') { return $entries }
        if ($entries.ContainsKey($name)) { throw "Duplicate CPIO entry: $name" }
        $entries[$name] = @{ Mode = $mode; Length = $length }
        $offset = ($offset + $length + 3) -band -4
    }
    throw 'Missing CPIO trailer.'
}

try
{
    $script:manifest = [ordered]@{
        formatVersion = 1; protocolVersion = 2; eventAbiVersion = 1; architecture = 'x86_64'
        loaderRevision = ('0' * 40); libbpfRevision = ('0' * 40)
        loaderDiffSha256 = ('0' * 64); libbpfDiffSha256 = ('0' * 64)
        loaderArchiveSha256 = ('0' * 64); libbpfArchiveSha256 = ('0' * 64)
        sourceDigest = ''; sourceFiles = @{}; files = @{}
    }
    $identity = ''
    $paths = @(Get-Content (Join-Path $PSScriptRoot 'SourceIdentity.cmake') |
        Where-Object { $_ -match '^\s+(src|tools)/' } |
        ForEach-Object { $_.Trim().TrimEnd(')') })
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    foreach ($path in $paths)
    {
        $text = [IO.File]::ReadAllText((Join-Path $sourceRoot $path.Replace('/', '\'))).Replace("`r`n", "`n")
        $sourceSha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sourceSha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-', '').ToLowerInvariant() }
        finally { $sourceSha.Dispose() }
        $script:manifest.sourceFiles[$path] = $hash
        $identity += "$hash  $path`n"
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $script:manifest.sourceDigest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    $static = New-Elf 62 -Executable
    [IO.File]::WriteAllBytes($collectorPath, $static)
    $script:manifest.files['wsl-audit-collector'] = ''
    foreach ($program in @('ebpf_wrapper_kern_raw_tp', 'ebpf_wrapper_kern_raw_tp_sub4096', 'ebpf_wrapper_kern_raw_tp_args64_krsi'))
    {
        $path = "wsl-security-audit/${program}_btf_rbuff_enriched_fmap.o"
        [IO.File]::WriteAllBytes((Join-Path $work $path.Replace('/', '\')), (New-Elf 247))
        $script:manifest.files[$path] = ''
    }
    [IO.File]::WriteAllText((Join-Path $work 'wsl-security-audit\syscalls.conf'), '# parser fixture')
    $script:manifest.files['wsl-security-audit/syscalls.conf'] = ''
    Write-Manifest
    & $validator -PayloadDirectory $work -Architecture x86_64 -SourceRoot $sourceRoot
    Write-Host 'PASS: static fixture inventory and source provenance'
    Expect-Rejection 'architecture mismatch' { & $validator -PayloadDirectory $work -Architecture aarch64 }
    $modified = $static.Clone()
    Set-U32 $modified 64 3
    [IO.File]::WriteAllBytes($collectorPath, $modified)
    Write-Manifest
    Expect-Rejection 'PT_INTERP' { & $validator -PayloadDirectory $work -Architecture x86_64 }
    $modified = [byte[]]::new(152)
    $static.CopyTo($modified, 0)
    Set-U32 $modified 64 2
    Set-U64 $modified 96 32
    Set-U64 $modified 120 1
    [IO.File]::WriteAllBytes($collectorPath, $modified)
    Write-Manifest
    Expect-Rejection 'DT_NEEDED' { & $validator -PayloadDirectory $work -Architecture x86_64 }
    [IO.File]::WriteAllBytes($collectorPath, $static)
    Expect-Rejection 'changed file hash' { & $validator -PayloadDirectory $work -Architecture x86_64 }
    Write-Manifest
    $extra = Join-Path $work 'libebpf_loader.so'
    [IO.File]::WriteAllText($extra, 'not a delivered artifact')
    Expect-Rejection 'unlisted shared runtime' { & $validator -PayloadDirectory $work -Architecture x86_64 }
    Remove-Item -LiteralPath $extra
    $originalDigest = $script:manifest.sourceDigest
    $script:manifest.sourceDigest = '0' * 64
    Write-Manifest
    Expect-Rejection 'provenance digest' { & $validator -PayloadDirectory $work -Architecture x86_64 }
    $script:manifest.sourceDigest = $originalDigest
    Write-Manifest
    $init = "$work.init-fixture"
    $archive = "$work.initrd-fixture"
    if ((Test-Path $init) -or (Test-Path $archive)) { throw 'Initrd fixture paths already exist.' }
    try
    {
        [IO.File]::WriteAllBytes($init, [byte[]]@(1, 2, 3))
        & (Join-Path $PSScriptRoot '..\create-initrd.ps1') -InputFile $init -OutputFile $archive
        $legacy = Read-Cpio $archive
        if ($legacy.Count -ne 1 -or $legacy['init'].Mode -ne 0x81ed) { throw 'Legacy initrd behavior changed.' }
        & (Join-Path $PSScriptRoot '..\create-initrd.ps1') -InputFile $init -OutputFile $archive `
            -AuditPayloadDirectory $work -Architecture x86_64
        $entries = Read-Cpio $archive
        if ($entries.Count -ne 8 -or $entries['init'].Length -ne 3 -or
            $entries['wsl-audit-collector'].Mode -ne 0x81ed -or
            $entries['wsl-security-audit/provenance.json'].Mode -ne 0x8124)
        {
            throw 'Trusted initrd payload inventory or permissions changed.'
        }
        Write-Host 'PASS: legacy /init preserved; collector, BPF, configuration and provenance packaged'
    }
    finally
    {
        Remove-Item -LiteralPath $init, $archive -Force -ErrorAction SilentlyContinue
    }
}
finally
{
    Remove-Item -LiteralPath $work -Recurse -Force
}
