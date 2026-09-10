# Copyright (C) Microsoft Corporation. All rights reserved.

param(
    [Parameter(Mandatory = $true)]
    [string]$WslResource,

    [Parameter(Mandatory = $true)]
    [string]$WslcResource,

    [Parameter(Mandatory = $true)]
    [string]$OutFile
)

$ErrorActionPreference = "Stop"

function LoadResource
{
    param([string]$Path)

    $document = New-Object System.Xml.XmlDocument
    $document.PreserveWhitespace = $false
    $document.Load((Resolve-Path -LiteralPath $Path).Path)

    if ($null -eq $document.SelectSingleNode("/root"))
    {
        throw "Resource file '$Path' does not contain a root element."
    }

    return $document
}

$wslDocument = LoadResource -Path $WslResource
$wslcDocument = LoadResource -Path $WslcResource
$resourceNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)

foreach ($entry in $wslDocument.SelectNodes("/root/data"))
{
    if (!$resourceNames.Add($entry.GetAttribute("name")))
    {
        throw "Duplicate resource '$($entry.GetAttribute("name"))' in '$WslResource'."
    }
}

foreach ($entry in $wslcDocument.SelectNodes("/root/data"))
{
    $name = $entry.GetAttribute("name")
    if (!$resourceNames.Add($name))
    {
        throw "Duplicate resource '$name' across '$WslResource' and '$WslcResource'."
    }

    [void]$wslDocument.DocumentElement.AppendChild($wslDocument.ImportNode($entry, $true))
}

$outputDirectory = Split-Path -Parent $OutFile
if (![string]::IsNullOrEmpty($outputDirectory))
{
    [void](New-Item -ItemType Directory -Force -Path $outputDirectory)
}

$settings = New-Object System.Xml.XmlWriterSettings
$settings.Encoding = New-Object System.Text.UTF8Encoding($false)
$settings.Indent = $true
$settings.IndentChars = "  "
$settings.NewLineChars = "`r`n"
$settings.NewLineHandling = [System.Xml.NewLineHandling]::Replace

$writer = [System.Xml.XmlWriter]::Create($OutFile, $settings)
try
{
    $wslDocument.Save($writer)
}
finally
{
    $writer.Dispose()
}
