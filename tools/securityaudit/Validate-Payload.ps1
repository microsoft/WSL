# Copyright (C) Microsoft Corporation. All rights reserved.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PayloadDirectory,
    [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture,
    [Parameter(Mandatory)][string]$SourceDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$payload = (Resolve-Path -LiteralPath $PayloadDirectory).Path
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$manifest = Get-Content -LiteralPath (Join-Path $payload 'payload.json') -Raw | ConvertFrom-Json
if ($manifest.formatVersion -ne 1 -or $manifest.architecture -cne $Architecture) {
    throw 'The audit payload format or architecture does not match this build.'
}
$loader = if ($Architecture -eq 'x64') { 'ld-linux-x86-64.so.2' } else { 'ld-linux-aarch64.so.1' }
$syscalls = if ($Architecture -eq 'x64') { 'syscalls.conf' } else { 'syscalls_aarch64.conf' }
$required = @('wsl_audit_collector', 'libebpf_loader.so', $syscalls, "runtime/$loader",
    'runtime/libc.so.6', 'runtime/libc++.so.1', 'runtime/libc++abi.so.1',
    'licenses/WSL-NOTICE.txt', 'licenses/nlohmann-json.txt')
foreach ($variant in 'raw_tp_sub4096', 'raw_tp', 'raw_tp_args64_krsi') {
    $required += "ebpf_wrapper_kern_${variant}_btf_rbuff_enriched_fmap.o"
}
$names = @($manifest.files.PSObject.Properties.Name)
foreach ($name in $required) {
    if ($names -cnotcontains $name) { throw "Required audit payload file is absent: $name" }
}
$expectedMachine = if ($Architecture -eq 'x64') { 62 } else { 183 }
foreach ($entry in $manifest.files.PSObject.Properties) {
    if ($entry.Name -cnotmatch '^(?:[A-Za-z0-9_.+-]+|(?:runtime|licenses)/[A-Za-z0-9_.+-]+)$' -or
        $entry.Name -match '(^|/)\.\.?($|/)' -or $entry.Value -cnotmatch '^[a-f0-9]{64}$') {
        throw "Invalid audit payload manifest entry: $($entry.Name)"
    }
    $path = Join-Path $payload $entry.Name.Replace('/', '\')
    if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Audit payload must contain regular files, not links: $path"
    }
    $stream = [IO.File]::OpenRead($path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
    if ($hash -cne $entry.Value) {
        throw "Audit payload hash mismatch: $path"
    }
    if ($entry.Name -eq 'wsl_audit_collector' -or $entry.Name -eq 'libebpf_loader.so' -or $entry.Name.StartsWith('runtime/')) {
        $stream = [IO.File]::OpenRead($path)
        try {
            $header = New-Object byte[] 20
            if ($stream.Read($header, 0, 20) -ne 20 -or
                [BitConverter]::ToString($header, 0, 6) -ne '7F-45-4C-46-02-01' -or
                [BitConverter]::ToUInt16($header, 18) -ne $expectedMachine) {
                throw "Audit payload contains an incompatible ELF binary: $path"
            }
        } finally { $stream.Dispose() }
    }
}
$actual = @(Get-ChildItem -LiteralPath $payload -Recurse -File |
    ForEach-Object { $_.FullName.Substring($payload.Length + 1).Replace('\', '/') } |
    Where-Object { $_ -cne 'payload.json' })
if (@(Compare-Object $names $actual -CaseSensitive).Count -ne 0) {
    throw 'Audit payload has missing or unmanifested files.'
}
$sourceNames = @($manifest.sources.PSObject.Properties.Name)
$expectedSources = @('packages.config', 'src/linux/securityaudit/CMakeLists.txt',
    'src/linux/securityaudit/Collector.h', 'tools/securityaudit/StagePayload.cmake.in',
    'tools/securityaudit/build-collector.sh')
$expectedSources += @(Get-ChildItem (Join-Path $source 'src\linux\securityaudit') -Filter '*.cpp' -File |
    ForEach-Object { "src/linux/securityaudit/$($_.Name)" })
$expectedSources += @(Get-ChildItem (Join-Path $source 'src\shared\securityaudit') -Filter '*.h' -File |
    ForEach-Object { "src/shared/securityaudit/$($_.Name)" })
if (@(Compare-Object $sourceNames $expectedSources -CaseSensitive).Count -ne 0) {
    throw 'The audit payload source manifest is incomplete or contains unexpected sources.'
}
foreach ($entry in $manifest.sources.PSObject.Properties) {
    if ($entry.Name -cnotmatch '^(src/(linux|shared)/securityaudit|tools/securityaudit)/[A-Za-z0-9_.-]+$|^packages\.config$') {
        throw "Invalid source identity path: $($entry.Name)"
    }
    $text = [IO.File]::ReadAllText((Join-Path $source $entry.Name.Replace('/', '\'))).Replace("`r`n", "`n")
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    if ($hash -cne $entry.Value) { throw "Stale audit payload; rebuild after changing $($entry.Name)" }
}
Write-Output "Validated $Architecture audit payload ($($names.Count) files)."
