# Copyright (C) Microsoft Corporation. All rights reserved.

[CmdletBinding()]
param(
    [AllowEmptyString()][string]$PayloadDirectory,
    [Parameter(Mandatory)][ValidateSet('x86_64', 'aarch64')][string]$Architecture,
    [string]$SdkVersion,
    [switch]$BuildInputs,
    [switch]$AllowDevelopmentPackage,
    [string]$StageDirectory,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PayloadHash.ps1')
function Assert-Range([byte[]]$Bytes, [uint64]$Offset, [uint64]$Length) {
    if ($Offset -gt $Bytes.LongLength -or $Length -gt $Bytes.LongLength - $Offset) {
        throw 'Truncated ELF/archive data.'
    }
}
function Assert-RelocatableElf([byte[]]$Bytes, [uint64]$Offset, [uint64]$Length, [uint16]$Machine) {
    Assert-Range $Bytes $Offset $Length
    if ($Length -lt 64 -or $Bytes[$Offset] -ne 0x7f -or $Bytes[$Offset+1] -ne 0x45 -or
        $Bytes[$Offset+2] -ne 0x4c -or $Bytes[$Offset+3] -ne 0x46 -or
        $Bytes[$Offset+4] -ne 2 -or $Bytes[$Offset+5] -ne 1 -or
        [BitConverter]::ToUInt16($Bytes, [int]$Offset+16) -ne 1 -or
        [BitConverter]::ToUInt16($Bytes, [int]$Offset+18) -ne $Machine) {
        throw "Expected a little-endian ELF64 relocatable object for machine $Machine."
    }
}
function Assert-StaticArchive([string]$Path, [uint16]$Machine) {
    if ((Get-Item -LiteralPath $Path).Length -gt 256MB) { throw 'Static archive exceeds its size limit.' }
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-Range $bytes 0 8
    if ([Text.Encoding]::ASCII.GetString($bytes,0,8) -cne "!<arch>`n") { throw "Not a static archive: $Path" }
    [uint64]$position = 8
    $objects = 0
    while ($position -lt $bytes.LongLength) {
        Assert-Range $bytes $position 60
        $header = [Text.Encoding]::ASCII.GetString($bytes,[int]$position,60)
        if ($header.Substring(58,2) -cne ([string][char]96 + "`n")) { throw 'Invalid archive member header.' }
        $name = $header.Substring(0,16).Trim()
        $sizeText = $header.Substring(48,10).Trim()
        [uint64]$size = 0
        if ($sizeText -cnotmatch '\A[0-9]+\z' -or -not [uint64]::TryParse($sizeText,[ref]$size)) {
            throw 'Invalid archive member size.'
        }
        $position += 60
        Assert-Range $bytes $position $size
        if ($name -cnotin @('/','//','/SYM64/')) {
            Assert-RelocatableElf $bytes $position $size $Machine
            $objects++
        }
        $position += $size
        if ($position % 2) { Assert-Range $bytes $position 1; $position++ }
    }
    if ($objects -eq 0) { throw "Static archive contains no ELF objects: $Path" }
}

if ([string]::IsNullOrWhiteSpace($PayloadDirectory) -or -not (Test-Path -LiteralPath $PayloadDirectory -PathType Container)) {
    throw 'Set WSL_SECURITYAUDIT_LIBRARY_DIR to a matching musl static audit package; dynamic collector packages are not supported.'
}
$root = (Get-Item -LiteralPath $PayloadDirectory).FullName.TrimEnd('\','/')
$entries = @((Get-Item -LiteralPath $root)) + @(Get-ChildItem -LiteralPath $root -Recurse -Force)
foreach ($entry in $entries) {
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package links are forbidden: $($entry.FullName)" }
}
$manifest = Get-Content (Join-Path $root 'audit-payload.json') -Raw | ConvertFrom-Json
if ($manifest.formatVersion -ne 3 -or $manifest.linkage -cne 'static-init' -or
    $manifest.architecture -cne $Architecture -or $manifest.protocolVersion -ne 2 -or $manifest.eventAbiVersion -ne 1) {
    throw 'Unsupported init audit package format, architecture or ABI.'
}
if ($manifest.developmentOnly -isnot [bool] -or ($manifest.developmentOnly -and -not $AllowDevelopmentPackage)) {
    throw 'Development audit archives require explicit approval and are not eligible for an official release.'
}
if ($SdkVersion -and $manifest.sdkVersion -cne $SdkVersion) { throw 'Audit package and init SDK versions differ.' }
if ($manifest.developmentOnly) { Write-Warning 'Using locally built musl archives for development, not an approved release artifact.' }
foreach ($name in 'loaderRevision','libbpfRevision') {
    if ($manifest.$name -cnotmatch '\A[0-9a-f]{40}\z') { throw "Invalid revision: $name" }
}
foreach ($name in 'sdkArchiveSha256','loaderPatchSha256','syscallSourceSha256') {
    if ($manifest.$name -cnotmatch '\A[0-9a-f]{64}\z') { throw "Invalid source provenance: $name" }
}
$syscall = if ($Architecture -eq 'aarch64') { 'syscalls_aarch64.conf' } else { 'syscalls.conf' }
$runtime = @(
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_sub4096_btf_rbuff_enriched_fmap.o',
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_btf_rbuff_enriched_fmap.o',
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_args64_krsi_btf_rbuff_enriched_fmap.o',
    "wsl-security-audit/$syscall"
)
$build = @('include/event_defs.h','include/ebpf_arch_macros.h','include/ebpf_plugin_loader.h',
    'lib/libebpf_loader.a','lib/libbpf.a','lib/libelf.a','lib/libeu.a','lib/libz.a')
if (Compare-Object ($runtime | Sort-Object) (@($manifest.files.PSObject.Properties.Name) | Sort-Object)) {
    throw 'Runtime payload must contain only the three BPF programs and the syscall table.'
}
if (Compare-Object ($build | Sort-Object) (@($manifest.buildFiles.PSObject.Properties.Name) | Sort-Object)) {
    throw 'Static build-input inventory is incomplete or unexpected.'
}
$expected = $runtime + 'audit-payload.json'
if ($BuildInputs) { $expected += $build }
$actual = @($entries | Where-Object { -not $_.PSIsContainer } |
    ForEach-Object { $_.FullName.Substring($root.Length+1).Replace('\','/') })
if (Compare-Object ($expected | Sort-Object) ($actual | Sort-Object)) {
    throw 'Unexpected/missing files; no separate collector executable or dynamic runtime is allowed.'
}
foreach ($relative in $expected | Where-Object { $_ -ne 'audit-payload.json' }) {
    $hash = if ($relative -cin $runtime) { $manifest.files.$relative } else { $manifest.buildFiles.$relative }
    $path = Join-Path $root $relative.Replace('/','\')
    if ($hash -cnotmatch '\A[0-9a-f]{64}\z' -or (Get-Item -LiteralPath $path).Length -eq 0 -or
        (Get-PayloadFileSha256 $path) -cne $hash) { throw "Package hash/size mismatch: $relative" }
}
$machine = if ($Architecture -eq 'aarch64') { 183 } else { 62 }
if ($BuildInputs) {
    foreach ($relative in $build | Where-Object { $_.EndsWith('.a') }) {
        Assert-StaticArchive (Join-Path $root $relative.Replace('/','\')) $machine
    }
    $abi = @(Select-String -LiteralPath (Join-Path $root 'include\event_defs.h') -Pattern '^#define VERSION 1$')
    if ($abi.Count -ne 1) { throw 'Unexpected loader header ABI.' }
}
foreach ($relative in $runtime | Where-Object { $_.EndsWith('.o') }) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root $relative.Replace('/','\')))
    Assert-RelocatableElf $bytes 0 $bytes.LongLength 247
}
$config = Join-Path $root "wsl-security-audit\$syscall"
if ((Get-Item -LiteralPath $config).Length -gt 1MB) { throw 'Syscall table exceeds its size limit.' }
$text = [IO.File]::ReadAllText($config,[Text.UTF8Encoding]::new($false,$true))
if ($text.Contains("`r")) { throw 'Syscall table must use LF, not CRLF.' }
$numbers = @{}
$rows = @($text.Split("`n") | Where-Object { $_ -ne '' -and -not $_.StartsWith('#') })
foreach ($line in $rows) {
    if ($line -cnotmatch '\A(0|[1-9][0-9]*)=[A-Za-z_][A-Za-z0-9_]*,[0-6]\z') {
        throw 'Malformed syscall table; Git symlink path-text placeholders are not valid.'
    }
    [int]$number = 0
    if (-not [int]::TryParse($Matches[1],[ref]$number) -or $numbers.ContainsKey($number)) { throw 'Invalid/duplicate syscall number.' }
    $numbers[$number] = $true
}
$anchors = if ($Architecture -eq 'aarch64') { @('63=read,3','64=write,3','221=execve,3') }
           else { @('0=read,3','1=write,3','59=execve,3') }
foreach ($anchor in $anchors) {
    if ($rows -cnotcontains $anchor) { throw "Incomplete or wrong-architecture syscall table: missing $anchor" }
}
if ($StageDirectory) {
    if ([IO.Path]::GetFullPath($StageDirectory).TrimEnd('\','/') -eq $root) { throw 'Input and staging directories must differ.' }
    if (Test-Path -LiteralPath $StageDirectory) {
        foreach ($entry in @((Get-Item -LiteralPath $StageDirectory)) + @(Get-ChildItem -LiteralPath $StageDirectory -Recurse -Force)) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Staging contains a link.' }
        }
    }
    $null = New-Item -ItemType Directory -Path (Join-Path $StageDirectory 'wsl-security-audit') -Force
    foreach ($relative in $runtime + 'audit-payload.json') {
        Copy-Item -LiteralPath (Join-Path $root $relative.Replace('/','\')) `
            -Destination (Join-Path $StageDirectory $relative.Replace('/','\')) -Force
    }
    & $PSCommandPath -PayloadDirectory $StageDirectory -Architecture $Architecture -SdkVersion $SdkVersion `
        -AllowDevelopmentPackage:$AllowDevelopmentPackage
}
Write-Verbose "Validated static init audit package for $Architecture, SDK $($manifest.sdkVersion)."
if ($PassThru) { $manifest }
