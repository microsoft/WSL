<#
.SYNOPSIS
    Builds a custom test registry image using wslc and saves it as a .tar file.
.DESCRIPTION
    This script builds a custom image using wslc from a specified Dockerfile and saves the resulting image as a .tar file. 
    This is useful for preparing test images for WSL container tests.
.PARAMETER DockerfileDir
    Path to the directory containing the Dockerfile to build.
.PARAMETER ImageTag
    Tag for the built image.
.PARAMETER OutputFile
    Path to save the exported .tar file. Defaults to <DockerfileDir name>.tar in the current directory.
#>

[CmdletBinding(SupportsShouldProcess)]
param (
    [string]$DockerfileDir,
    [string]$ImageTag,
    [string]$OutputFile = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($OutputFile -eq "") {
    $OutputFile = Join-Path $PWD "$(Split-Path -Leaf $DockerfileDir).tar"
}

# Verify $OutputFile is a valid path, we can write to it, and that it has a .tar extension
if ([System.IO.Path]::GetExtension($OutputFile) -ne ".tar") {
    if (-not $PSCmdlet.ShouldContinue("Are you sure you want to continue?", "Output file '$OutputFile' is not a .tar file.")) {
        throw "Aborting due to invalid output file extension."
    }
}


if ($PSCmdlet.ShouldProcess($ImageTag, "Build image from '$DockerfileDir'")) {
    & wslc build -t $ImageTag $DockerfileDir
    if ($LASTEXITCODE -ne 0) { throw "wslc build failed with exit code $LASTEXITCODE" }
}

if ($PSCmdlet.ShouldProcess($OutputFile, "Save image '$ImageTag'")) {
    & wslc save --output $OutputFile $ImageTag
    if ($LASTEXITCODE -ne 0) { throw "wslc save failed with exit code $LASTEXITCODE" }
}

Write-Host "Image built and saved to $OutputFile successfully."
