# Copyright (C) Microsoft Corporation. All rights reserved.
#
# Generates seed corpus files for each fuzzing harness.
# Output is deterministic (no randomness) — run in the pipeline before staging.
# Output goes into per-target subdirectories matching harness names.

param(
    [Parameter(Mandatory)]
    [string]$OutputDir
)

# Write a wide (UTF-16LE) null-terminated string
function Write-WideString([System.IO.BinaryWriter]$writer, [string]$s) {
    $writer.Write([System.Text.Encoding]::Unicode.GetBytes($s + "`0"))
}

# Write a narrow (UTF-8) null-terminated string
function Write-NarrowString([System.IO.BinaryWriter]$writer, [string]$s) {
    $writer.Write([System.Text.Encoding]::UTF8.GetBytes($s + "`0"))
}

# Create a seed binary file, invoking $body with a BinaryWriter
function New-SeedFile([string]$dir, [string]$file, [scriptblock]$body) {
    New-Item -ItemType Directory -Path $dir -Force -ErrorAction Stop | Out-Null
    
    # File::Create() resolves relative paths differently than PowerShell, so we use a full path.
    $path = Join-Path (Resolve-Path $dir) $file

    $stream = [System.IO.File]::Create($path)
    $writer = [System.IO.BinaryWriter]::new($stream)
    try {
        & $body $writer
    }
    finally {
        $writer.Close()
    }
}

# --- CLI harness seeds ---
# Format: concatenated null-terminated wide strings (argv-style, split by SplitToArgs)

function New-CliSeed([string]$name, [string[]]$cliArgs) {
    New-SeedFile "$OutputDir\WslcCliArgumentFuzzing\seeds" "$name.bin" {
        param($writer)
        foreach ($arg in $cliArgs) {
            Write-WideString $writer $arg
        }
    }
}

New-CliSeed "install" @("--install", "Ubuntu")
New-CliSeed "run-command" @("--name", "MyDistro", "--user", "root", "--", "ls", "-la")
New-CliSeed "flags" @("--all", "--verbose", "--force")
New-CliSeed "env-volume" @("--env", "FOO=bar", "--env", "BAZ=qux", "--volume", "C:\Users:/mnt/users", "--workdir", "/home/user")
New-CliSeed "publish" @("--publish", "8080:80", "--publish", "443:443", "--hostname", "devbox")

# --- SDK and WinRT harness seeds (same binary format) ---
# Format: [widestr sessionName] [widestr storagePath]
#          [str imageName] [str containerName] [str hostName]
#          [u8 portCount] [portCount x (u16 windowsPort, u16 containerPort, u8 protocol)]
#          [u32 flags]

$sdkSeedDir = "$OutputDir\WslcSdkFuzzing\seeds"

function New-SdkSeed {
    param(
        [string]$Name,
        [string]$SessionName,
        [string]$StoragePath,
        [string]$ImageName,
        [string]$ContainerName,
        [string]$HostName,
        [hashtable[]]$Ports = @(),
        [uint32]$Flags = 0
    )
    New-SeedFile $sdkSeedDir "$Name.bin" {
        param($writer)
        Write-WideString $writer $SessionName
        Write-WideString $writer $StoragePath
        Write-NarrowString $writer $ImageName
        Write-NarrowString $writer $ContainerName
        Write-NarrowString $writer $HostName
        $writer.Write([byte]$Ports.Count)
        foreach ($port in $Ports) {
            $writer.Write([uint16]$port.WindowsPort)
            $writer.Write([uint16]$port.ContainerPort)
            $writer.Write([byte]$port.Protocol)
        }
        $writer.Write([uint32]$Flags)
    }
}

New-SdkSeed -Name "basic-session" `
    -SessionName "test-session" -StoragePath "C:\temp\storage" `
    -ImageName "ubuntu:latest" -ContainerName "my-container" -HostName "localhost" `
    -Ports @{WindowsPort=8080; ContainerPort=80; Protocol=0}, @{WindowsPort=443; ContainerPort=443; Protocol=1} `
    -Flags 0

New-SdkSeed -Name "empty-strings" `
    -SessionName "" -StoragePath "" `
    -ImageName "" -ContainerName "" -HostName ""

New-SdkSeed -Name "long-names" `
    -SessionName ("A" * 63) -StoragePath ("C:\very\long\path\" + ("x" * 40)) `
    -ImageName ("registry.example.com/org/" + ("img" * 10) + ":v1.2.3") `
    -ContainerName ("container-" + ("name" * 10)) -HostName ("host-" + ("name" * 10)) `
    -Ports @{WindowsPort=9090; ContainerPort=9090; Protocol=0} `
    -Flags ([uint32]::MaxValue)

# WinRT uses the same seeds
$winrtSeedDir = "$OutputDir\WslcWinRtFuzzing\seeds"
New-Item -ItemType Directory -Path $winrtSeedDir -Force | Out-Null
Copy-Item -Path "$sdkSeedDir\*" -Destination $winrtSeedDir -Force

Write-Host "Seeds generated in: $OutputDir"
Get-ChildItem -Recurse -File -Path $OutputDir | ForEach-Object {
    Write-Host "  $(Resolve-Path $_.FullName -Relative)"
}
