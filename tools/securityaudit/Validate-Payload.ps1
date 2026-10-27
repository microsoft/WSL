# Copyright (C) Microsoft Corporation. All rights reserved.

[CmdletBinding()]
param(
    [AllowEmptyString()][string]$PayloadDirectory,
    [Parameter(Mandatory)][ValidateSet('x86_64', 'aarch64')][string]$Architecture,
    [string]$SdkVersion,
    [string]$PackageVersion,
    [switch]$BuildInputs,
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
    throw 'Audit payload directory is missing. Restore the architecture-specific ebpf.linux-musl NuGet package before building.'
}
$root = (Get-Item -LiteralPath $PayloadDirectory).FullName.TrimEnd('\','/')
$entries = @((Get-Item -LiteralPath $root)) + @(Get-ChildItem -LiteralPath $root -Recurse -Force)
foreach ($entry in $entries) {
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package links are forbidden: $($entry.FullName)" }
}
$syscall = if ($Architecture -eq 'aarch64') { 'syscalls_aarch64.conf' } else { 'syscalls.conf' }
$packageId = if ($Architecture -eq 'aarch64') { 'ebpf.linux-musl-arm64' } else { 'ebpf.linux-musl-x86_64' }
$rid = if ($Architecture -eq 'aarch64') { 'arm64' } else { 'x64' }
$runtime = @(
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_sub4096_btf_rbuff_enriched_fmap.o',
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_btf_rbuff_enriched_fmap.o',
    'wsl-security-audit/ebpf_wrapper_kern_raw_tp_args64_krsi_btf_rbuff_enriched_fmap.o',
    "wsl-security-audit/$syscall"
)
$archives = @('ebpf_loader','bpf','elf','eu','z') | ForEach-Object { "runtimes/linux-musl-$rid/native/lib$_.a" }
$build = @('include/ebpf/event_defs.h','include/ebpf/ebpf_arch_macros.h','include/ebpf/ebpf_plugin_loader.h') + $archives
$paths = @{}
$hashes = [ordered]@{}
$inputs = [ordered]@{}
if ($BuildInputs) {
    if (-not $SdkVersion -or -not $PackageVersion) { throw 'SDK and package versions are required when validating NuGet build inputs.' }
    $metadataPath = Join-Path $root 'metadata\build.json'
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    $link = Get-Content -LiteralPath (Join-Path $root 'metadata\link.json') -Raw | ConvertFrom-Json
    # packages.config restores keep the nupkg, but do not extract its nuspec.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $package = [IO.Compression.ZipFile]::OpenRead((Join-Path $root "$packageId.$PackageVersion.nupkg"))
    try {
        $entry = $package.GetEntry("$packageId.nuspec")
        if (-not $entry) { throw 'Audit NuGet package is missing its nuspec.' }
        $reader = [IO.StreamReader]::new($entry.Open())
        try { $nuspec = [xml]$reader.ReadToEnd() }
        finally { $reader.Dispose() }
    } finally { $package.Dispose() }
    if ($nuspec.package.metadata.id -cne $packageId -or $nuspec.package.metadata.version -cne $PackageVersion) {
        throw 'Audit NuGet package identity or version does not match packages.config.'
    }
    if ($metadata.target -cne "$Architecture-unknown-linux-musl" -or $link.target -cne $metadata.target) {
        throw 'Audit NuGet package target does not match the build architecture.'
    }
    if ($metadata.sdk -cne "Microsoft.WSL.LinuxSdk/$SdkVersion" -or $link.sdk -cne $metadata.sdk) {
        throw 'Audit package and init SDK versions differ.'
    }
    if (($link.archives -join ';') -cne ($archives -join ';')) { throw 'Unexpected audit static link dependencies or order.' }
    foreach ($relative in $runtime) {
        $source = 'include/ebpf/' + ($relative -split '/')[-1]
        $paths[$relative] = $source
        $hashes[$relative] = $metadata.sha256.$source
        $inputs[$source] = $metadata.sha256.$source
    }
    foreach ($relative in $build) {
        $inputs[$relative] = $metadata.sha256.$relative
    }
    $manifest = [pscustomobject]@{
        formatVersion = 4
        linkage = 'static-init'
        architecture = $Architecture
        protocolVersion = 2
        eventAbiVersion = 1
        packageId = $packageId
        packageVersion = $PackageVersion
        sdkVersion = $SdkVersion
        loaderRevision = $metadata.source_commit
        sdkArchiveSha256 = $metadata.dependencies.sdk.sha256
        packageBuildSha256 = Get-PayloadFileSha256 $metadataPath
        files = [pscustomobject]$hashes
    }
} else {
    $manifest = Get-Content -LiteralPath (Join-Path $root 'audit-payload.json') -Raw | ConvertFrom-Json
    $expected = $runtime + 'audit-payload.json'
    $actual = @($entries | Where-Object { -not $_.PSIsContainer } |
        ForEach-Object { $_.FullName.Substring($root.Length+1).Replace('\','/') })
    if (Compare-Object ($expected | Sort-Object) ($actual | Sort-Object)) {
        throw 'Unexpected/missing runtime files; no separate collector executable or dynamic runtime is allowed.'
    }
    foreach ($relative in $runtime) {
        $paths[$relative] = $relative
        $inputs[$relative] = $manifest.files.$relative
    }
}
if ($manifest.formatVersion -ne 4 -or $manifest.linkage -cne 'static-init' -or
    $manifest.architecture -cne $Architecture -or $manifest.protocolVersion -ne 2 -or $manifest.eventAbiVersion -ne 1) {
    throw 'Unsupported init audit package format, architecture or ABI.'
}
if ($manifest.packageId -cne $packageId -or [string]::IsNullOrWhiteSpace($manifest.packageVersion) -or
    ($PackageVersion -and $manifest.packageVersion -cne $PackageVersion)) {
    throw 'Unexpected audit NuGet package identity or version.'
}
if ($SdkVersion -and $manifest.sdkVersion -cne $SdkVersion) { throw 'Audit package and init SDK versions differ.' }
if ($manifest.loaderRevision -cnotmatch '\A[0-9a-f]{40}\z') { throw 'Invalid loader revision.' }
foreach ($name in 'sdkArchiveSha256','packageBuildSha256') {
    if ($manifest.$name -cnotmatch '\A[0-9a-f]{64}\z') { throw "Invalid source provenance: $name" }
}
if (Compare-Object ($runtime | Sort-Object) (@($manifest.files.PSObject.Properties.Name) | Sort-Object)) {
    throw 'Runtime payload must contain only the three BPF programs and the syscall table.'
}
foreach ($relative in $inputs.Keys) {
    $hash = $inputs[$relative]
    $path = Join-Path $root $relative.Replace('/','\')
    if ($hash -cnotmatch '\A[0-9a-f]{64}\z' -or (Get-Item -LiteralPath $path).Length -eq 0 -or
        (Get-PayloadFileSha256 $path) -cne $hash) { throw "Package hash/size mismatch: $relative" }
}
$machine = if ($Architecture -eq 'aarch64') { 183 } else { 62 }
if ($BuildInputs) {
    foreach ($relative in $archives) {
        Assert-StaticArchive (Join-Path $root $relative.Replace('/','\')) $machine
    }
    $abi = @(Select-String -LiteralPath (Join-Path $root 'include\ebpf\event_defs.h') -Pattern '^#define VERSION 1$')
    if ($abi.Count -ne 1) { throw 'Unexpected loader header ABI.' }
}
foreach ($relative in $runtime | Where-Object { $_.EndsWith('.o') }) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root $paths[$relative].Replace('/','\')))
    Assert-RelocatableElf $bytes 0 $bytes.LongLength 247
}
$config = Join-Path $root $paths["wsl-security-audit/$syscall"].Replace('/','\')
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
    foreach ($relative in $runtime) {
        Copy-Item -LiteralPath (Join-Path $root $paths[$relative].Replace('/','\')) `
            -Destination (Join-Path $StageDirectory $relative.Replace('/','\')) -Force
    }
    [IO.File]::WriteAllText((Join-Path $StageDirectory 'audit-payload.json'),
        ($manifest | ConvertTo-Json -Depth 5) + "`n", [Text.UTF8Encoding]::new($false))
    & $PSCommandPath -PayloadDirectory $StageDirectory -Architecture $Architecture -SdkVersion $SdkVersion `
        -PackageVersion $PackageVersion
}
Write-Verbose "Validated static init audit package for $Architecture, SDK $($manifest.sdkVersion)."
if ($PassThru) { $manifest }
