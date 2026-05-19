# Copyright (C) Microsoft Corporation. All rights reserved.

<#
.SYNOPSIS
    Builds the Microsoft.WSL.Containers NuGet package locally for testing.

.DESCRIPTION
    Updates the cmake cache with WSL_NUGET_SINGLE_ARCH=ON (so only the already-
    configured platform's binaries are required), then invokes the nuget_containers
    cmake target to produce a .nupkg suitable for local testing.

    The target platform is whatever cmake was last configured for (e.g. x64 or
    arm64). Run cmake . -A arm64 first if you want an arm64 package.

    The output package is written to <repo-root>\out\nuget\.

.PARAMETER Config
    Build configuration: Debug or Release. Defaults to Debug.

.PARAMETER Version
    NuGet package version string. Defaults to the git-computed version.

.PARAMETER Fast
    Skip the cmake reconfiguration step. Use this on subsequent runs when
    cmake cache variables haven't changed, to iterate quickly.

.EXAMPLE
    .\tools\build-nuget.ps1
    Builds a Debug package for the currently configured platform.

.EXAMPLE
    .\tools\build-nuget.ps1 -Fast
    Skips cmake config and packs immediately (fastest iteration).

.EXAMPLE
    .\tools\build-nuget.ps1 -Config Release -Version 0.0.1-local
    Builds a Release package with version 0.0.1-local.
#>

param(
    [ValidateSet("Debug", "Release")]
    [string] $Config = "Debug",

    [string] $Version = "",

    [switch] $Fast
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent
Push-Location $RepoRoot

try {
    # Update the cmake cache variables we need without touching the generator or
    # platform (those are already set from the user's initial cmake . run).
    if (-not $Fast) {
        $cmakeArgs = @(
            ".",
            "-DCMAKE_BUILD_TYPE=$Config",
            "-DWSL_NUGET_SINGLE_ARCH=ON"
        )

        if ($Version -ne "") {
            $cmakeArgs += "-DWSL_NUGET_PACKAGE_VERSION=$Version"
        }

        Write-Host "Updating cmake cache for $Config build..." -ForegroundColor Cyan
        cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit code $LASTEXITCODE)" }
    }

    Write-Host "`nBuilding wslcsdkwinrtidl (generates winmd)..." -ForegroundColor Cyan
    cmake --build . --config $Config --target wslcsdkwinrtidl -- -m
    if ($LASTEXITCODE -ne 0) { throw "wslcsdkwinrtidl build failed (exit code $LASTEXITCODE)" }

    # NOTE: The C# WinRT projection (wslcsdkcs.dll) is not built here. Building it
    # requires a full Visual Studio installation's MSBuild (not Build Tools) to resolve
    # Microsoft.NET.Sdk. To include it: run cmake with -DWSL_BUILD_SDKCS=ON, build
    # the wslcsdkcs target in Visual Studio, then re-run this script with -Fast.

    Write-Host "`nPacking NuGet package..." -ForegroundColor Cyan
    cmake --build . --config $Config --target nuget_containers
    if ($LASTEXITCODE -ne 0) { throw "nuget_containers target failed (exit code $LASTEXITCODE)" }

    $OutDir = Join-Path $RepoRoot "out\nuget"
    $Package = Get-ChildItem -Path $OutDir -Filter "Microsoft.WSL.Containers.*.nupkg" |
               Sort-Object LastWriteTime -Descending |
               Select-Object -First 1

    if ($Package) {
        Write-Host "`nPackage ready: $($Package.FullName)" -ForegroundColor Green
    }
    else {
        Write-Warning "Package build succeeded but no .nupkg found in $OutDir"
    }
}
finally {
    Pop-Location
}
