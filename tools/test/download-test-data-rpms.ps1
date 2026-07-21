
[CmdletBinding(PositionalBinding = $False)]
param (
    [Parameter(Mandatory = $true)][string]$Target,
    [string]$BaseUrl = "https://packages.microsoft.com/azurelinux/3.0/prod/base"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$ProgressPreference = "SilentlyContinue"

$archMap = @{
    "x64"   = "x86_64"
    "arm64" = "aarch64"
}

$packages = @("socat", "readline", "ncurses-libs")

function Get-LatestRpmUrl {
    param (
        [string]$RepoUrl,
        [string]$Arch,
        [string]$Package
    )

    $letter = $Package.Substring(0, 1).ToLowerInvariant()
    $listingUrl = "$RepoUrl/$Arch/Packages/$letter/"

    $listing = curl.exe --fail -sSL $listingUrl
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to list packages at $listingUrl"
    }

    $pattern = 'href="(?<file>' + [regex]::Escape($Package) + '-(?<ver>[0-9][0-9.]*)-(?<rel>[0-9]+)\.azl[0-9]+\.' + $Arch + '\.rpm)"'
    $matches = [regex]::Matches($listing, $pattern)
    if ($matches.Count -eq 0) {
        throw "No '$Package' rpm found for '$Arch' at $listingUrl"
    }

    $latest = $matches |
        Sort-Object -Property `
            @{ Expression = { [version]$_.Groups["ver"].Value } }, `
            @{ Expression = { [int]$_.Groups["rel"].Value } } |
        Select-Object -Last 1

    return "$listingUrl$($latest.Groups['file'].Value)"
}

foreach ($archEntry in $archMap.GetEnumerator()) {
    $nugetArch = $archEntry.Key
    $repoArch = $archEntry.Value

    $archInput = Join-Path $Target $nugetArch
    if (-not (Test-Path -Path $archInput -PathType Container)) {
        Write-Warning "Skipping '$nugetArch': '$archInput' does not exist."
        continue
    }

    $packagesDir = Join-Path $archInput "packages"
    New-Item -ItemType Directory -Path $packagesDir -Force | Out-Null

    foreach ($package in $packages) {
        $url = Get-LatestRpmUrl -RepoUrl $BaseUrl -Arch $repoArch -Package $package
        $fileName = Split-Path -Path $url -Leaf
        $destination = Join-Path $packagesDir $fileName

        Write-Output "[$nugetArch] Downloading $url"
        curl.exe --fail -sSL -o $destination $url
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to download $url"
        }
    }
}