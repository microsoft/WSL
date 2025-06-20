[cmdletbinding(PositionalBinding = $false)]
param (
    [string]$VmName,
    [string]$Username,
    [string]$Password,
    [ValidateSet("X64", "arm64")][string]$Platform = "X64",
    [ValidateSet("Debug", "Release")][string]$BuildType = "Debug",
    [string]$RemoteTempFolder = "C:\\",
    [string]$BuildOutputPath = [string](Get-Location),
    [parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraMsiArgs
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrEmpty($Password)) {
    $SecurePassword = New-Object System.Security.SecureString
}
else {
    $SecurePassword = ConvertTo-SecureString "$Password" -AsPlainText -Force
}

$Credential = New-Object System.Management.Automation.PSCredential("$Username", $SecurePassword)
$Session = New-PSSession -VMName $VmName -Credential $Credential

$Package = $BuildOutputPath + "/bin/$Platform/$BuildType/wsl.msi"
Copy-Item -ToSession $Session -Path $Package -Destination "$RemoteTempFolder" -Force


Invoke-Command -Session $Session -ScriptBlock {

    $MSIArguments = @(
    "/i"
    "$using:RemoteTempFolder\wsl.msi"
    "/qn"
    "/norestart"
    )
    
    if ($using:ExtraMsiArgs)
    {
        $MSIArguments += $using:ExtraMsiArgs
    }

    $exitCode = (Start-Process -Wait "msiexec.exe" -ArgumentList $MSIArguments -NoNewWindow -PassThru).ExitCode
    if ($exitCode -Ne 0)
    {
        Write-Host "Failed to install package: $exitCode"
        exit 1
    }

    Write-Host "Package $using:Package successfully deployed on $using:VmName"
}

Remove-PSSession -Session $Session