# Fast unit test runner for WSLC CLI - requires administrator privileges
param(
    [string]$Filter = "*WSLCCLI*UnitTests*",
    [string]$Configuration = "debug",
    [string]$Platform = "x64",
    [string]$Version = "2",  # WSL version to test (1 or 2)
    [switch]$SkipDistroCheck  # Skip checking for test distro (for tests that don't need it)
)

# Function to check if running as administrator.
function Test-Administrator {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentUser)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Check if we need elevation.
if (-not (Test-Administrator)) {
    Write-Host ""
    Write-Host "ERROR: This script requires Administrator privileges." -ForegroundColor Red
    Write-Host ""
    Write-Host "Please run this script from an elevated PowerShell prompt:" -ForegroundColor Yellow
    Write-Host "  1. Right-click PowerShell and select 'Run as Administrator'" -ForegroundColor Cyan
    Write-Host "  2. Navigate to: $PSScriptRoot" -ForegroundColor Cyan
    Write-Host "  3. Run: .\run_unit_tests.ps1" -ForegroundColor Cyan
    Write-Host ""
    exit 1
}

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
Push-Location $repoRoot

try {
    $testDll = "bin\$Platform\$Configuration\wsltests.dll"

    # Check if test DLL exists
    if (-not (Test-Path $testDll)) {
        Write-Host ""
        Write-Host "ERROR: Test DLL not found: $testDll" -ForegroundColor Red
        Write-Host ""
        Write-Host "Please build the project first:" -ForegroundColor Yellow
        Write-Host "  cmake --build . -- -m" -ForegroundColor Cyan
        Write-Host ""
        exit 1
    }

    # Find TAEF in the NuGet packages directory
    $packagesConfig = Join-Path $repoRoot "packages.config"
    if (-not (Test-Path $packagesConfig)) {
        Write-Host "ERROR: packages.config not found at: $packagesConfig" -ForegroundColor Red
        exit 1
    }

    # Parse package versions from packages.config
    [xml]$packagesXml = Get-Content $packagesConfig
    
    # Get TAEF package info
    $taefPackage = $packagesXml.packages.package | Where-Object { $_.id -eq "Microsoft.Taef" }
    if ($null -eq $taefPackage) {
        Write-Host "ERROR: Microsoft.Taef package not found in packages.config" -ForegroundColor Red
        exit 1
    }

    $taefVersion = $taefPackage.version
    $taefPath = Join-Path $repoRoot "packages\Microsoft.Taef.$taefVersion\build\Binaries\$Platform"
    $teExe = Join-Path $taefPath "te.exe"

    # Check if TAEF test runner exists
    if (-not (Test-Path $teExe)) {
        Write-Host ""
        Write-Host "ERROR: TAEF test runner not found: $teExe" -ForegroundColor Red
        Write-Host ""
        Write-Host "TAEF binaries haven't been downloaded yet." -ForegroundColor Yellow
        Write-Host ""
        Write-Host "Run cmake to restore NuGet packages:" -ForegroundColor Yellow
        Write-Host "  cmake ." -ForegroundColor Cyan
        Write-Host ""
        exit 1
    }

    # Get test distro package info
    $testDistroPackage = $packagesXml.packages.package | Where-Object { $_.id -eq "Microsoft.WSL.TestDistro" }
    $distroPath = ""
    
    if ($null -ne $testDistroPackage -and -not $SkipDistroCheck) {
        $testDistroVersion = $testDistroPackage.version
        $distroPath = Join-Path $repoRoot "packages\Microsoft.WSL.TestDistro.$testDistroVersion\test_distro.tar.xz"
        
        if (-not (Test-Path $distroPath)) {
            Write-Host ""
            Write-Host "WARNING: Test distro not found at: $distroPath" -ForegroundColor Yellow
            Write-Host ""
            Write-Host "Some tests may fail without the test distribution." -ForegroundColor Yellow
            Write-Host "To skip this check, use: -SkipDistroCheck" -ForegroundColor Gray
            Write-Host ""
            
            # Ask user if they want to continue
            $response = Read-Host "Continue anyway? (y/N)"
            if ($response -ne 'y' -and $response -ne 'Y') {
                exit 1
            }
            $distroPath = ""
        }
    }

    # Get test data package info
    $testDataPackage = $packagesXml.packages.package | Where-Object { $_.id -eq "Microsoft.WSL.TestData" }
    $testDataPath = ""
    
    if ($null -ne $testDataPackage) {
        $testDataVersion = $testDataPackage.version
        $testDataPath = Join-Path $repoRoot "packages\Microsoft.WSL.TestData.$testDataVersion\build\native\bin\x64"
    }

    Write-Host "`nRunning WSLC CLI unit tests" -ForegroundColor Cyan
    Write-Host "Filter: $Filter" -ForegroundColor Gray
    Write-Host "Configuration: $Configuration" -ForegroundColor Gray
    Write-Host "Platform: $Platform" -ForegroundColor Gray
    Write-Host "WSL Version: $Version" -ForegroundColor Gray
    Write-Host "TAEF: $teExe" -ForegroundColor Gray
    if ($distroPath) {
        Write-Host "Test Distro: $distroPath" -ForegroundColor Gray
    }
    Write-Host ""

    # Build the command line arguments
    $teArgs = @(
        $testDll
        "/name:$Filter"
        "/p:Version=$Version"
        "/p:SetupScript="
        "/p:DistroPath=$distroPath"
        "/p:TestDataPath=$testDataPath"
        "/p:Package="
        "/p:UnitTestsPath="
        "/p:PullRequest=false"
        "/p:AllowUnsigned=1"
    )

    # Run tests with required runtime parameters
    & $teExe @teArgs

    $exitCode = $LASTEXITCODE
    
    Write-Host ""
    if ($exitCode -eq 0) {
        Write-Host "Tests passed!" -ForegroundColor Green
    } else {
        Write-Host "Tests failed with exit code: $exitCode" -ForegroundColor Red
    }
    
    Write-Host ""
    exit $exitCode
}
finally {
    Pop-Location
}