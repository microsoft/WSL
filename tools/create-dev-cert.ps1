[CmdletBinding()]
param (
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
# Set PSModulePath environment variable explicitly to work with PowerShell 7
$env:PSModulePath = [Environment]::GetEnvironmentVariable('PSModulePath', 'Machine')

$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" -KeyUsage DigitalSignature -FriendlyName "WSL Dev cert" -CertStoreLocation "Cert:\CurrentUser\My" -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") -HashAlgorithm "SHA256" -NotAfter (Get-Date).AddYears(10)

Export-PfxCertificate -cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" -FilePath $OutputPath -Password (New-Object System.Security.SecureString) | Out-Null
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)"  -DeleteKey | Out-Null

Write-Host "Created new dev certificate in $OutputPath"