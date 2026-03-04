<#
.SYNOPSIS
    Creates initrd.img from the init binary using CPIO newc format.
.DESCRIPTION
    This script packages the init binary into a CPIO archive for use as an initrd.
.PARAMETER InitPath
    Path to the init binary.
.PARAMETER OutputPath
    Path for the output initrd.img file.
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$InitPath,
    
    [Parameter(Mandatory=$true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $InitPath)) {
    Write-Error "Init binary not found at: $InitPath"
    exit 1
}

$initData = [System.IO.File]::ReadAllBytes($InitPath)
$initSize = $initData.Length
$mtime = [int][double]::Parse((Get-Date -UFormat %s))

# CPIO newc header format helper
function Write-CpioHeader {
    param($stream, $name, $size, $mode, $nlink, $mtime)
    
    $nameBytes = [System.Text.Encoding]::ASCII.GetBytes($name + [char]0)
    $nameLen = $nameBytes.Length
    $headerPadding = (4 - ((110 + $nameLen) % 4)) % 4
    $dataPadding = (4 - ($size % 4)) % 4
    
    # CPIO newc header: 110 bytes
    $header = [string]::Format(
        "070701" +           # magic
        "{0:X8}" +           # inode
        "{1:X8}" +           # mode
        "{2:X8}" +           # uid
        "{3:X8}" +           # gid
        "{4:X8}" +           # nlink
        "{5:X8}" +           # mtime
        "{6:X8}" +           # filesize
        "{7:X8}" +           # devmajor
        "{8:X8}" +           # devminor
        "{9:X8}" +           # rdevmajor
        "{10:X8}" +          # rdevminor
        "{11:X8}" +          # namesize
        "{12:X8}",           # check
        0,                   # inode
        $mode,               # mode (0100755 = regular file, rwxr-xr-x)
        0,                   # uid
        0,                   # gid
        $nlink,              # nlink
        $mtime,              # mtime
        $size,               # filesize
        0,                   # devmajor
        0,                   # devminor
        0,                   # rdevmajor
        0,                   # rdevminor
        $nameLen,            # namesize
        0                    # check
    )
    
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
    $stream.Write($headerBytes, 0, $headerBytes.Length)
    $stream.Write($nameBytes, 0, $nameBytes.Length)
    
    # Pad to 4-byte alignment
    for ($i = 0; $i -lt $headerPadding; $i++) {
        $stream.WriteByte(0)
    }
    
    return $dataPadding
}

try {
    $stream = [System.IO.File]::Create($OutputPath)
    
    # Write init file entry
    $dataPadding = Write-CpioHeader -stream $stream -name "init" -size $initSize -mode 0x81ED -nlink 1 -mtime $mtime
    $stream.Write($initData, 0, $initData.Length)
    
    # Pad data to 4-byte alignment
    for ($i = 0; $i -lt $dataPadding; $i++) {
        $stream.WriteByte(0)
    }
    
    # Write TRAILER!!! entry
    $null = Write-CpioHeader -stream $stream -name "TRAILER!!!" -size 0 -mode 0 -nlink 0 -mtime $mtime
    
    $stream.Close()
    
    Write-Host "Created initrd.img at: $OutputPath"
}
catch {
    Write-Error "Failed to create initrd: $_"
    if ($stream) { $stream.Close() }
    exit 1
}
