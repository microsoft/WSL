# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

# Creates a CPIO newc initramfs containing init and its optional eBPF resources.

[CmdletBinding()]
param (
    [Parameter(Mandatory, Position = 0)][string]$InputFile,
    [Parameter(Mandatory, Position = 1)][string]$OutputFile,
    [Parameter(Position = 2, ValueFromRemainingArguments)][string[]]$AuditPayloadFiles = @()
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

$out = [System.IO.File]::Create($OutputFile)
try
{
    Write-CpioEntry $out $name $data 0x81ED $mtime  # S_IFREG | 0755
    if ($AuditPayloadFiles.Count -gt 0)
    {
        Write-CpioEntry $out ([Text.Encoding]::ASCII.GetBytes("wsl-security-audit`0")) ([byte[]]::new(0)) 0x41ED $mtime
        foreach ($file in $AuditPayloadFiles)
        {
            $bytes = [IO.File]::ReadAllBytes($file)
            $name = [IO.Path]::GetFileName($file)
            Write-CpioEntry $out ([Text.Encoding]::ASCII.GetBytes("wsl-security-audit/$name`0")) $bytes 0x8124 $mtime  # S_IFREG | 0444
        }
    }
    $trailer = [System.Text.Encoding]::ASCII.GetBytes("TRAILER!!!`0")
    Write-CpioEntry $out $trailer ([byte[]]::new(0)) 0 0
}
finally
{
    $out.Close()
}
