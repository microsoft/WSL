# Copyright (C) Microsoft Corporation. All rights reserved.

[CmdletBinding()]
param(
    [AllowEmptyString()][string]$PayloadDirectory,
    [Parameter(Mandatory)][ValidateSet('x86_64', 'aarch64')][string]$Architecture,
    [string]$SourceRoot,
    [string]$StageDirectory,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PayloadHash.ps1')

function Assert-Range([byte[]]$Bytes, [uint64]$Offset, [uint64]$Length)
{
    if ($Offset -gt $Bytes.LongLength -or $Length -gt $Bytes.LongLength - $Offset)
    {
        throw 'Truncated or invalid ELF bounds.'
    }
}

function Assert-Elf([string]$Path, [uint16]$Machine, [switch]$Executable)
{
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -gt 256MB)
    {
        throw "ELF exceeds the payload size limit: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    Assert-Range $bytes 0 64
    if ($bytes[0] -ne 0x7f -or $bytes[1] -ne 0x45 -or $bytes[2] -ne 0x4c -or $bytes[3] -ne 0x46 -or
        $bytes[4] -ne 2 -or $bytes[5] -ne 1 -or $bytes[6] -ne 1 -or
        [BitConverter]::ToUInt16($bytes, 18) -ne $Machine -or [BitConverter]::ToUInt16($bytes, 52) -ne 64)
    {
        throw "Expected little-endian ELF64 machine $Machine`: $Path"
    }
    $type = [BitConverter]::ToUInt16($bytes, 16)
    if (-not $Executable)
    {
        if ($type -ne 1) { throw "BPF payload is not a relocatable object: $Path" }
        return
    }
    if ($type -ne 2 -and $type -ne 3) { throw "Not an ELF executable: $Path" }
    $offset = [BitConverter]::ToUInt64($bytes, 32)
    $size = [BitConverter]::ToUInt16($bytes, 54)
    $count = [BitConverter]::ToUInt16($bytes, 56)
    if ($size -ne 56 -or $count -eq 0 -or $count -gt 4096) { throw 'Invalid ELF program headers.' }
    Assert-Range $bytes $offset ([uint64]$size * $count)
    $entry = [BitConverter]::ToUInt64($bytes, 24)
    $executableEntry = $false
    for ($index = 0; $index -lt $count; ++$index)
    {
        $header = [int]($offset + $index * $size)
        $segment = [BitConverter]::ToUInt32($bytes, $header)
        $fileOffset = [BitConverter]::ToUInt64($bytes, $header + 8)
        $length = [BitConverter]::ToUInt64($bytes, $header + 32)
        Assert-Range $bytes $fileOffset $length
        if ($segment -eq 3) { throw "Dynamic ELF interpreter (PT_INTERP) is forbidden: $Path" }
        if ($segment -eq 1 -and ([BitConverter]::ToUInt32($bytes, $header + 4) -band 1))
        {
            $address = [BitConverter]::ToUInt64($bytes, $header + 16)
            if ($entry -ge $address -and $entry - $address -lt $length) { $executableEntry = $true }
        }
        if ($segment -eq 2)
        {
            if ($length % 16 -ne 0) { throw 'Invalid ELF dynamic segment.' }
            $terminated = $false
            for ([uint64]$position = $fileOffset; $position -lt $fileOffset + $length; $position += 16)
            {
                $tag = [BitConverter]::ToUInt64($bytes, [int]$position)
                if ($tag -eq 0) { $terminated = $true; break }
                if ($tag -eq 1) { throw "Shared library dependency (DT_NEEDED) is forbidden: $Path" }
            }
            if (-not $terminated) { throw 'Unterminated ELF dynamic segment.' }
        }
    }
    if (-not $executableEntry) { throw 'ELF entry point is not backed by executable file data.' }
}

if ([string]::IsNullOrWhiteSpace($PayloadDirectory) -or -not (Test-Path -LiteralPath $PayloadDirectory -PathType Container))
{
    throw 'Static audit payload missing. Build tools\securityaudit\build-payload.sh on Linux and set WSL_SECURITYAUDIT_PAYLOAD_DIR; a dynamic package is not supported.'
}
$root = (Get-Item -LiteralPath $PayloadDirectory).FullName
$entries = @((Get-Item -LiteralPath $root)) + @(Get-ChildItem -LiteralPath $root -Recurse -Force)
foreach ($entry in $entries)
{
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Payload links are forbidden: $($entry.FullName)" }
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'audit-payload.json') -Raw | ConvertFrom-Json
if ($manifest.formatVersion -ne 1 -or $manifest.protocolVersion -ne 2 -or $manifest.architecture -cne $Architecture -or
    $manifest.eventAbiVersion -ne 1)
{
    throw 'Unsupported audit payload manifest, protocol, event ABI, or architecture.'
}
foreach ($name in @('loaderRevision', 'libbpfRevision'))
{
    if ($manifest.$name -cnotmatch '^[0-9a-f]{40}$') { throw "Invalid payload provenance: $name" }
}
foreach ($name in @('sourceDigest', 'loaderDiffSha256', 'libbpfDiffSha256', 'loaderArchiveSha256', 'libbpfArchiveSha256'))
{
    if ($manifest.$name -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid payload provenance: $name" }
}
$syscalls = if ($Architecture -eq 'aarch64') { 'syscalls_aarch64.conf' } else { 'syscalls.conf' }
$expected = @(
    'wsl-audit-collector',
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_sub4096_btf_rbuff_enriched_fmap.o',
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_btf_rbuff_enriched_fmap.o',
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_args64_krsi_btf_rbuff_enriched_fmap.o',
    "wsl-security-audit/$syscalls"
)
$listed = @($manifest.files.PSObject.Properties.Name)
if (Compare-Object ($expected | Sort-Object) ($listed | Sort-Object)) { throw 'Payload inventory is incomplete or unexpected.' }
$actual = @($entries | Where-Object { -not $_.PSIsContainer } |
    ForEach-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') })
if (Compare-Object (($expected + 'audit-payload.json') | Sort-Object) ($actual | Sort-Object))
{
    throw 'Payload contains missing or unlisted files (shared runtimes and external plugin folders are forbidden).'
}
foreach ($path in $expected)
{
    $fullPath = Join-Path $root $path.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $hash = $manifest.files.$path
    if ($hash -cnotmatch '^[0-9a-f]{64}$' -or (Get-PayloadFileSha256 $fullPath) -cne $hash)
    {
        throw "Audit payload hash mismatch: $path"
    }
    if ((Get-Item -LiteralPath $fullPath).Length -eq 0) { throw "Empty audit payload file: $path" }
}
$machine = if ($Architecture -eq 'aarch64') { 183 } else { 62 }
Assert-Elf (Join-Path $root 'wsl-audit-collector') $machine -Executable
foreach ($path in $expected | Where-Object { $_.EndsWith('.o') })
{
    Assert-Elf (Join-Path $root $path.Replace('/', [IO.Path]::DirectorySeparatorChar)) 247
}
$sourcePaths = @(
    'src/linux/securityaudit/Collector.cpp', 'src/linux/securityaudit/Collector.h',
    'src/linux/securityaudit/CollectorMain.cpp', 'src/linux/securityaudit/AuditBuildInfo.h.in',
    'src/shared/securityaudit/ObservationSchema.h', 'src/shared/securityaudit/ProcessCreated.h',
    'src/shared/securityaudit/ProcessExecuted.h', 'src/shared/securityaudit/Protocol.h',
    'src/shared/securityaudit/SecurityEvents.h', 'tools/securityaudit/CMakeLists.txt',
    'tools/securityaudit/SourceIdentity.cmake', 'tools/securityaudit/GenerateBuildInfo.cmake',
    'tools/securityaudit/StagePayload.cmake.in', 'tools/securityaudit/VerifyElf.cmake', 'tools/securityaudit/build-payload.sh'
)
if (Compare-Object ($sourcePaths | Sort-Object) (@($manifest.sourceFiles.PSObject.Properties.Name) | Sort-Object))
{
    throw 'Unexpected collector source inventory.'
}
$identity = ''
[Array]::Sort($sourcePaths, [StringComparer]::Ordinal)
foreach ($path in $sourcePaths)
{
    $hash = $manifest.sourceFiles.$path
    if ($hash -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid collector source hash: $path" }
    if ($SourceRoot)
    {
        $source = Join-Path $SourceRoot $path.Replace('/', [IO.Path]::DirectorySeparatorChar)
        $text = [IO.File]::ReadAllText($source).Replace("`r`n", "`n")
        $sourceSha = [Security.Cryptography.SHA256]::Create()
        try { $sourceHash = [BitConverter]::ToString($sourceSha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-', '').ToLowerInvariant() }
        finally { $sourceSha.Dispose() }
        if ($sourceHash -cne $hash)
        {
            throw "Static audit payload is stale: $path. Rebuild it from the current core sources."
        }
    }
    $identity += "$hash  $path`n"
}
$sha = [Security.Cryptography.SHA256]::Create()
try { $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
if ($digest -cne $manifest.sourceDigest) { throw 'Collector source provenance digest mismatch.' }
if ($StageDirectory)
{
    if ([IO.Path]::GetFullPath($StageDirectory).TrimEnd('\', '/') -eq $root.TrimEnd('\', '/'))
    {
        throw 'Payload input and staging directories must be different.'
    }
    if (Test-Path -LiteralPath $StageDirectory)
    {
        foreach ($entry in @((Get-Item -LiteralPath $StageDirectory)) + @(Get-ChildItem -LiteralPath $StageDirectory -Recurse -Force))
        {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Staging links are forbidden: $($entry.FullName)" }
        }
    }
    $null = New-Item -ItemType Directory -Path (Join-Path $StageDirectory 'wsl-security-audit') -Force
    foreach ($path in $expected + 'audit-payload.json')
    {
        $relative = $path.Replace('/', [IO.Path]::DirectorySeparatorChar)
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination (Join-Path $StageDirectory $relative) -Force
    }
    & $PSCommandPath -PayloadDirectory $StageDirectory -Architecture $Architecture -SourceRoot $SourceRoot
}
Write-Verbose "Validated fully static $Architecture audit collector and BPF payload ($($manifest.loaderRevision))."
if ($PassThru) { $manifest }
