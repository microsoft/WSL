[CmdletBinding(PositionalBinding = $false)]
param (
    [Parameter(Mandatory)]
    [string]$Version,
    [string]$Distribution
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$WorkingDirectory = Join-Path $PSScriptRoot "test-data"
$OutputDirectory = $PWD.Path

$wslArguments = @()
if (-not [string]::IsNullOrEmpty($Distribution)) {
    $wslArguments += @("--distribution", $Distribution)
}
$wslArguments += "--exec"

function Invoke-WslCommand {
    param (
        [Parameter(Mandatory = $true)][string[]]$Command,
        [switch]$CaptureOutput
    )

    if ($CaptureOutput) {
        $output = & wsl.exe @wslArguments @Command
        if ($LASTEXITCODE -ne 0) {
            throw "WSL command failed with exit code $LASTEXITCODE`: $($Command -join ' ')"
        }

        return $output
    }

    & wsl.exe @wslArguments @Command
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code $LASTEXITCODE`: $($Command -join ' ')"
    }
}

function ConvertTo-WslPath {
    param (
        [Parameter(Mandatory = $true)][string]$Path
    )

    $result = Invoke-WslCommand -Command @("wslpath", "-a", $Path) -CaptureOutput
    return ($result | Select-Object -Last 1).Trim()
}

function Enable-ForeignArchitectureBuilds {
    $nativeArchitecture = (
        Invoke-WslCommand -Command @("docker", "version", "--format", "{{.Server.Arch}}") -CaptureOutput |
            Select-Object -Last 1
    ).Trim()

    foreach ($architecture in @("amd64", "arm64")) {
        if ($architecture -eq $nativeArchitecture) {
            continue
        }

        Write-Host "Installing Docker binfmt support for $architecture"
        Invoke-WslCommand -Command @(
            "docker", "container", "run",
            "--privileged",
            "--rm",
            "tonistiigi/binfmt",
            "--install", $architecture
        )
    }
}

function Download-TestDataRpms {
    param (
        [Parameter(Mandatory = $true)][string]$NugetArchitecture,
        [Parameter(Mandatory = $true)][string]$DockerArchitecture
    )

    $packagesDirectory = Join-Path $WorkingDirectory "$NugetArchitecture\packages"
    New-Item -ItemType Directory -Path $packagesDirectory -Force | Out-Null
    $packagesPath = ConvertTo-WslPath $packagesDirectory

    Write-Host "[$NugetArchitecture] Downloading Azure Linux RPMs"
    $downloadCommand = @(
        "tdnf reinstall -y --downloadonly --downloaddir=/packages readline ncurses-libs",
        "tdnf install -y --downloadonly --downloaddir=/packages socat"
    ) -join " && "

    Invoke-WslCommand -Command @(
        "docker", "container", "run",
        "--rm",
        "--platform", "linux/$DockerArchitecture",
        "--volume", "${packagesPath}:/packages",
        "mcr.microsoft.com/azurelinux/base/core:3.0",
        "sh", "-c", $downloadCommand
    )

    $downloadedPackages = @(Get-ChildItem -LiteralPath $packagesDirectory -Filter "*.rpm" -File)
    if ($downloadedPackages.Count -ne 3) {
        throw "Expected 3 RPMs for $NugetArchitecture, found $($downloadedPackages.Count)."
    }
}

function Export-DockerImage {
    param (
        [Parameter(Mandatory = $true)][string]$Image,
        [Parameter(Mandatory = $true)][string]$DockerArchitecture,
        [Parameter(Mandatory = $true)][string]$OutputPath,
        [string]$Source
    )

    if ([string]::IsNullOrEmpty($Source)) {
        $Source = "docker-daemon:$Image"
    }

    $outputDirectory = Split-Path -Parent $OutputPath
    $outputFileName = Split-Path -Leaf $OutputPath
    $wslOutputDirectory = ConvertTo-WslPath $outputDirectory

    Invoke-WslCommand -Command @(
        "docker", "container", "run",
        "--rm",
        "--volume", "/var/run/docker.sock:/var/run/docker.sock",
        "--volume", "${wslOutputDirectory}:/output",
        "quay.io/skopeo/stable",
        "copy",
        "--override-arch", $DockerArchitecture,
        $Source,
        "docker-archive:/output/${outputFileName}:$Image"
    )

    # Adjust the tar padding to match docker's behavior so the "wslc image save" tests match the size of the tars in the testdata package.
    $archive = Get-Item -LiteralPath $OutputPath
    [long]$tarRecordSize = 10KB
    $paddedLength = [Math]::Ceiling($archive.Length / $tarRecordSize) * $tarRecordSize
    if ($paddedLength -ne $archive.Length) {
        $stream = [System.IO.File]::OpenWrite($archive.FullName)
        try {
            $stream.SetLength($paddedLength)
        }
        finally {
            $stream.Dispose()
        }
    }
}

function Export-TestImages {
    param (
        [Parameter(Mandatory = $true)][string]$NugetArchitecture,
        [Parameter(Mandatory = $true)][string]$DockerArchitecture
    )

    $architectureDirectory = Join-Path $WorkingDirectory $NugetArchitecture
    New-Item -ItemType Directory -Path $architectureDirectory -Force | Out-Null

    $images = [ordered]@{
        "alpine:latest"      = "alpine-latest.tar"
        "debian:latest"      = "debian-latest.tar"
        "hello-world:latest" = "HelloWorldSaved.tar"
        "python:3.12-alpine" = "python-3_12-alpine.tar"
    }

    foreach ($entry in $images.GetEnumerator()) {
        Write-Host "[$NugetArchitecture] Downloading $($entry.Key)"
        Export-DockerImage `
            -Image $entry.Key `
            -DockerArchitecture $DockerArchitecture `
            -OutputPath (Join-Path $architectureDirectory $entry.Value) `
            -Source "docker://docker.io/library/$($entry.Key)"
    }

    Invoke-WslCommand -Command @("docker", "image", "pull", "--platform", "linux/$DockerArchitecture", "hello-world:latest")

    $containerId = $null
    try {
        $containerId = (
            Invoke-WslCommand -Command @(
                "docker", "container", "create", "--platform", "linux/$DockerArchitecture", "hello-world:latest"
            ) -CaptureOutput |
                Select-Object -Last 1
        ).Trim()

        $exportPath = ConvertTo-WslPath (Join-Path $architectureDirectory "HelloWorldExported.tar")
        Invoke-WslCommand -Command @("docker", "container", "export", "--output", $exportPath, $containerId)
    }
    finally {
        if (-not [string]::IsNullOrEmpty($containerId)) {
            Invoke-WslCommand -Command @("docker", "container", "rm", "--force", $containerId)
        }
    }

    Write-Host "[$NugetArchitecture] Building wslc-registry:latest"
    $buildContext = ConvertTo-WslPath (Join-Path $PSScriptRoot "images\wslc-registry")
    Invoke-WslCommand -Command @(
        "docker", "image", "build",
        "--platform", "linux/$DockerArchitecture",
        "--pull",
        "--tag", "wslc-registry:latest",
        $buildContext
    )

    Export-DockerImage `
        -Image "wslc-registry:latest" `
        -DockerArchitecture $DockerArchitecture `
        -OutputPath (Join-Path $architectureDirectory "wslc-registry.tar")
}

try {
    if (Test-Path -LiteralPath $WorkingDirectory) {
        Remove-Item -LiteralPath $WorkingDirectory -Recurse -Force
    }

    New-Item -ItemType Directory -Path $WorkingDirectory -Force | Out-Null

    Write-Host "Checking Docker in WSL"
    Invoke-WslCommand -Command @("docker", "version")
    Enable-ForeignArchitectureBuilds

    Export-TestImages -NugetArchitecture "x64" -DockerArchitecture "amd64"
    Export-TestImages -NugetArchitecture "arm64" -DockerArchitecture "arm64"

    Download-TestDataRpms -NugetArchitecture "x64" -DockerArchitecture "amd64"
    Download-TestDataRpms -NugetArchitecture "arm64" -DockerArchitecture "arm64"

    $nuspecPath = Join-Path $WorkingDirectory "Microsoft.WSL.TestData.nuspec"
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "Microsoft.WSL.TestData.nuspec") -Destination $nuspecPath -Force

    Write-Host "Building test data NuGet. Input: $WorkingDirectory. Version: $Version"
    & (Join-Path $PSScriptRoot "..\..\_deps\nuget.exe") pack $nuspecPath `
        -Properties "version=$Version" `
        -OutputDirectory $OutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "NuGet pack failed with exit code $LASTEXITCODE."
    }
}
finally {
    if (Test-Path -LiteralPath $WorkingDirectory) {
        Remove-Item -LiteralPath $WorkingDirectory -Recurse -Force
    }
}
