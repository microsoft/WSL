# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

# Creates a CPIO newc initramfs with /init and optional audit resources from a restored NuGet package.

[CmdletBinding()]
param (
    [Parameter(Mandatory)][string]$InputFile,
    [Parameter(Mandatory)][string]$OutputFile,
    [string]$AuditPayloadDirectory,
    [ValidateSet('x86_64', 'aarch64')][string]$Architecture
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Pad([System.IO.Stream]$Stream)
{
    $remainder = $Stream.Position % 4
    if ($remainder -ne 0)
    {
        $pad = [byte[]]::new(4 - $remainder)
        $Stream.Write($pad, 0, $pad.Length)
    }
}

function Write-CpioEntry([System.IO.Stream]$Stream, [byte[]]$NameBytes, [byte[]]$FileData, [int]$Mode, [uint32]$Mtime)
{
    $header = "070701" +                        # header magic
              "00000001" +                      # inode
              ("{0:X8}" -f $Mode) +             # mode
              "00000000" +                      # uid
              "00000000" +                      # gid
              "00000001" +                      # nlink
              ("{0:X8}" -f $Mtime) +            # mtime
              ("{0:X8}" -f $FileData.Length) +  # filesize
              "00000000" +                      # devmajor
              "00000000" +                      # devminor
              "00000000" +                      # rdevmajor
              "00000000" +                      # rdevminor
              ("{0:X8}" -f $NameBytes.Length) + # namesize
              "00000000"                        # check
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
    $Stream.Write($headerBytes, 0, $headerBytes.Length)
    $Stream.Write($NameBytes, 0, $NameBytes.Length)
    Write-Pad $Stream
    if ($FileData.Length -gt 0)
    {
        $Stream.Write($FileData, 0, $FileData.Length)
        Write-Pad $Stream
    }
}

$data = [System.IO.File]::ReadAllBytes($InputFile)
$name = [System.Text.Encoding]::ASCII.GetBytes("init`0")
$mtime = [uint32][System.DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

$auditFiles = @{}
if ($AuditPayloadDirectory)
{
    if (-not $Architecture) { throw 'Architecture is required for the audit initrd payload.' }
    $syscall = if ($Architecture -eq 'aarch64') { 'syscalls_aarch64.conf' } else { 'syscalls.conf' }
    foreach ($file in @(
        'ebpf_wrapper_kern_raw_tp_sub4096_btf_rbuff_enriched_fmap.o',
        'ebpf_wrapper_kern_raw_tp_btf_rbuff_enriched_fmap.o',
        'ebpf_wrapper_kern_raw_tp_args64_krsi_btf_rbuff_enriched_fmap.o',
        $syscall))
    {
        $auditFiles[$file] = [IO.File]::ReadAllBytes((Join-Path $AuditPayloadDirectory $file))
    }
}

$out = [System.IO.File]::Create($OutputFile)
try
{
    Write-CpioEntry $out $name $data 0x81ED $mtime  # S_IFREG | 0755
    if ($auditFiles.Count -gt 0)
    {
        $directory = [Text.Encoding]::ASCII.GetBytes("wsl-security-audit`0")
        Write-CpioEntry $out $directory ([byte[]]::new(0)) 0x41ED $mtime # S_IFDIR | 0755
        foreach ($entry in $auditFiles.GetEnumerator() | Sort-Object Key)
        {
            $mode = 0x8124 # 0444: BPF programs and syscall data, not a second userspace executable.
            Write-CpioEntry $out ([Text.Encoding]::ASCII.GetBytes("wsl-security-audit/$($entry.Key)`0")) $entry.Value $mode $mtime
        }
    }
    $trailer = [System.Text.Encoding]::ASCII.GetBytes("TRAILER!!!`0")
    Write-CpioEntry $out $trailer ([byte[]]::new(0)) 0 0
}
finally
{
    $out.Close()
}
