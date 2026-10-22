# Copyright (C) Microsoft Corporation. All rights reserved.
#requires -Version 5.1

[CmdletBinding()]
param([string]$FixturesPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$readerRoot = [IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path)
if ([string]::IsNullOrEmpty($FixturesPath)) {
    $FixturesPath = Join-Path $readerRoot 'fixtures\native-security-fixtures.json'
}
. (Join-Path $readerRoot 'Read-SecurityObservation.ps1') -XmlPath ''

$script:rejections = 0
function Assert-Native([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-NativeEqual($Actual, $Expected, [string]$Path) {
    if ($null -eq $Actual -or $null -eq $Expected) {
        Assert-Native ($null -eq $Actual -and $null -eq $Expected) "$Path nullability differs."
    }
    elseif ($Expected -is [pscustomobject]) {
        Assert-Native ($Actual -is [pscustomobject]) "$Path must be an object."
        $names = @($Expected.PSObject.Properties.Name)
        Assert-Native (@($Actual.PSObject.Properties.Name).Count -eq $names.Count) "$Path field count differs."
        foreach ($name in $names) {
            Assert-Native (@($Actual.PSObject.Properties.Name) -ccontains $name) "$Path lacks $name."
            Assert-NativeEqual $Actual.$name $Expected.$name "$Path.$name"
        }
    }
    elseif ($Expected -is [array]) {
        Assert-Native ($Actual -is [array] -and $Actual.Count -eq $Expected.Count) "$Path array shape differs."
        for ($index = 0; $index -lt $Expected.Count; $index++) {
            Assert-NativeEqual $Actual[$index] $Expected[$index] "$Path[$index]"
        }
    }
    elseif ($Expected -is [string]) {
        Assert-Native ($Actual -is [string] -and $Actual -ceq $Expected) "$Path text differs."
    }
    elseif ($Expected -is [bool]) {
        Assert-Native ($Actual -is [bool] -and $Actual -eq $Expected) "$Path Boolean differs."
    }
    else {
        Assert-Native (($Actual -is [int] -or $Actual -is [long] -or $Actual -is [uint32] -or $Actual -is [uint64]) -and
            [decimal]$Actual -eq [decimal]$Expected) "$Path integer differs."
    }
}

function Assert-NativeRejected([scriptblock]$Action, [string]$Case) {
    $rejected = $false
    try { $null = & $Action } catch { $rejected = $true }
    Assert-Native $rejected "Accepted invalid fixture: $Case"
    $script:rejections++
}

function Read-FixtureXml([string]$XmlText) {
    $document = [Xml.XmlDocument]::new()
    $document.XmlResolver = $null
    $document.PreserveWhitespace = $true
    $document.LoadXml($XmlText)
    return ,$document
}

function Find-FixtureNodes([Xml.XmlDocument]$Document, [string]$Path) {
    $ns = [Xml.XmlNamespaceManager]::new($Document.NameTable)
    $ns.AddNamespace('e', 'http://schemas.microsoft.com/win/2004/08/events/event')
    return ,$Document.SelectNodes($Path, $ns)
}

function New-FixtureFields($Sample, [AllowEmptyString()][string[]]$Fields) {
    $document = Read-FixtureXml $Sample.xml
    $data = (Find-FixtureNodes $document '/e:Event/e:EventData')[0]
    $data.RemoveAll()
    foreach ($field in $Fields) {
        $node = $document.CreateElement('Data', $document.DocumentElement.NamespaceURI)
        $node.InnerText = $field
        $null = $data.AppendChild($node)
    }
    return $document.OuterXml.Replace("`r", '&#13;')
}

function Assert-NativeXmlRejected($Sample, [string]$Xml, [string]$Case) {
    Assert-NativeRejected { ConvertFrom-SecurityObservationXml -XmlText $Xml } "$Case (unified reader)"
    Assert-NativeRejected { Assert-SecurityObservationDescription -XmlText $Xml -Description $Sample.description } "$Case (General)"
    if ($Sample.eventId -in @(62010, 62011)) {
        Assert-NativeRejected { ConvertFrom-ProcessSecurityXml -XmlText $Xml } "$Case (process reader)"
    }
}

$parameters = @{ InputObject = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $FixturesPath).Path) }
if ($PSVersionTable.PSVersion -ge [version]'7.5') { $parameters.DateKind = 'String' }
$fixtureSet = ConvertFrom-Json @parameters
$ids = @(62010,62011,62020,62021,62022,62023,62024,62030,62031,62040,62041,62042)
Assert-Native ($fixtureSet.fixtureKind -ceq 'synthetic-not-live') 'Only explicitly synthetic fixtures may be used here.'
Assert-Native ($fixtureSet.samples.Count -eq 32) 'Incomplete fixture matrix.'
Assert-NativeEqual @($fixtureSet.supportedEventIds) $ids 'Acceptance IDs'

foreach ($id in $ids) {
    foreach ($variant in @('ordinary', 'known')) {
        Assert-Native (@($fixtureSet.samples | Where-Object { $_.eventId -eq $id -and $_.variant -ceq $variant }).Count -eq 1) `
            "Missing or duplicate $id/$variant fixture."
    }
}
foreach ($sample in $fixtureSet.samples) {
    $case = "$($sample.eventId)/$($sample.variant)"
    $actual = ConvertFrom-SecurityObservationXml -XmlText $sample.xml
    Assert-NativeEqual $actual $sample.expected $case
    Assert-SecurityObservationDescription -XmlText $sample.xml -Description $sample.description
    Assert-SecurityObservationDescription -XmlText $sample.xml -Description $sample.description.Replace("`r`n", "`n")
    if ($sample.eventId -in @(62010, 62011)) {
        Assert-NativeEqual (ConvertFrom-ProcessSecurityXml $sample.xml) $sample.expected "$case/process"
        Assert-ProcessSecurityDescription -XmlText $sample.xml -Description $sample.description
    }
    if ($sample.eventId -eq 62010) {
        Assert-ProcessExecutedDescription -XmlText $sample.xml -Description $sample.description
    }
    if ($sample.variant -cne 'ordinary') {
        Assert-Native ($actual.wslcSessionId -is [uint32]) "$case lost uint32 WSLC session identity."
        Assert-Native ($null -eq $actual.distroGuid -and $null -eq $actual.distroName) "$case guessed distribution identity."
    }
    # Optional Windows System metadata is preserved in the input and never confused with payload fields.
    $minimal = Read-FixtureXml $sample.xml
    foreach ($name in @('Computer', 'EventRecordID', 'TimeCreated', 'Execution', 'Security')) {
        foreach ($node in (Find-FixtureNodes $minimal "/e:Event/e:System/e:$name")) {
            $null = $node.ParentNode.RemoveChild($node)
        }
    }
    Assert-NativeEqual (ConvertFrom-SecurityObservationXml $minimal.OuterXml) $sample.expected "$case/minimal-metadata"
    Assert-SecurityObservationDescription -XmlText $minimal.OuterXml -Description $sample.description
    $badVersion = $sample.description.Replace('Event format version: ' + $sample.fields[0], 'Event format version: 99')
    Assert-NativeRejected { Assert-SecurityObservationDescription $sample.xml $badVersion } "$case/wrong-General-version"
}

$ordinary = @($fixtureSet.samples | Where-Object { $_.eventId -eq 62010 -and $_.variant -ceq 'ordinary' })[0]
$wslc = @($fixtureSet.samples | Where-Object { $_.eventId -eq 62010 -and $_.variant -ceq 'known' })[0]
$observation = @($fixtureSet.samples | Where-Object { $_.eventId -eq 62022 -and $_.variant -ceq 'known' })[0]
foreach ($sample in @($ordinary, $wslc, $observation)) {
    foreach ($provider in @('Other-Provider', 'WSL-SecurityAudit', ('WSL-SecurityAudit-Test-' + ('a' * 32)),
            ('WSL-SecurityAudit-Verify-' + ('a' * 32)), 'microsoft-windows-wsl-securityaudit',
            'Microsoft-Windows-WSL-SecurityAudit-Test')) {
        $document = Read-FixtureXml $sample.xml
        (Find-FixtureNodes $document '/e:Event/e:System/e:Provider')[0].SetAttribute('Name', $provider)
        Assert-NativeXmlRejected $sample $document.OuterXml "provider $provider"
    }
    foreach ($name in @('Provider', 'EventID', 'Version', 'Channel')) {
        $document = Read-FixtureXml $sample.xml
        $node = (Find-FixtureNodes $document "/e:Event/e:System/e:$name")[0]
        $null = $node.ParentNode.AppendChild($node.CloneNode($true))
        Assert-NativeXmlRejected $sample $document.OuterXml "duplicate System.$name"
    }
    foreach ($change in @(@('Channel','Application'), @('Version','1'))) {
        $document = Read-FixtureXml $sample.xml
        (Find-FixtureNodes $document ("/e:Event/e:System/e:" + $change[0]))[0].InnerText = $change[1]
        Assert-NativeXmlRejected $sample $document.OuterXml "wrong System.$($change[0])"
    }
    foreach ($unsupported in @(62001,62002,62025,62050,62051,62099)) {
        $document = Read-FixtureXml $sample.xml
        (Find-FixtureNodes $document '/e:Event/e:System/e:EventID')[0].InnerText = [string]$unsupported
        Assert-NativeXmlRejected $sample $document.OuterXml "unsupported ID $unsupported"
    }
    foreach ($fields in @(@($sample.fields[0..4]), (@($sample.fields) + @('extra')))) {
        Assert-NativeXmlRejected $sample (New-FixtureFields $sample $fields) 'wrong insertion count'
    }
    $fields = @($sample.fields); $fields[0] = '99'
    Assert-NativeXmlRejected $sample (New-FixtureFields $sample $fields) 'unsupported schema version'
    $fields = @($sample.fields); $fields[2] = 'x' * 16385
    Assert-NativeXmlRejected $sample (New-FixtureFields $sample $fields) 'oversized insertion'
    $fields = @($sample.fields); $fields[3] = 'x' * 16384; $fields[4] = 'x' * 16384
    Assert-NativeXmlRejected $sample (New-FixtureFields $sample $fields) 'oversized total payload'
    $fields = @($sample.fields); $fields[5] = $fields[5].Replace('18446744073709551615', '18446744073709551616')
    Assert-Native ($fields[5] -cne $sample.fields[5]) 'Fixture lacks the uint64 boundary clock.'
    Assert-NativeXmlRejected $sample (New-FixtureFields $sample $fields) 'uint64 overflow'
    $document = Read-FixtureXml $sample.xml
    $node = (Find-FixtureNodes $document '/e:Event/e:EventData/e:Data')[2]
    $null = $node.AppendChild($document.CreateElement('Nested', $document.DocumentElement.NamespaceURI))
    Assert-NativeXmlRejected $sample $document.OuterXml 'non-scalar insertion'
    foreach ($description in @($sample.description.Replace('Operation details','Wrong heading'),
            ($sample.description + $sample.fields[1]),
            ($sample.description.Replace($sample.fields[1], '')),
            ($sample.description + "`nData={}`n"), ('x' * 131073))) {
        Assert-NativeRejected { Assert-SecurityObservationDescription $sample.xml $description } 'invalid General rendering'
    }
}
$fields = @($wslc.fields); $fields[0] = '4'
Assert-NativeXmlRejected $wslc (New-FixtureFields $wslc $fields) 'WSLC identity with ordinary version'
$fields = @($ordinary.fields); $fields[0] = '5'
Assert-NativeXmlRejected $ordinary (New-FixtureFields $ordinary $fields) 'ordinary identity with WSLC version'
$fields = @($wslc.fields); $fields[1] = $ordinary.fields[1]
Assert-NativeXmlRejected $wslc (New-FixtureFields $wslc $fields) 'WSLC identity replaced with distribution'
Assert-NativeRejected { ConvertFrom-SecurityObservationXml ('x' * 131073) } 'oversized XML'
Assert-NativeRejected { ConvertFrom-ProcessSecurityXml ('x' * 131073) } 'oversized process XML'
Assert-NativeXmlRejected $ordinary ('<!DOCTYPE Event [<!ENTITY fixture "x">]>' + $ordinary.xml) 'DTD'

Write-Output ("PASS: {0} SYNTHETIC fixtures, {1} supported IDs, {2} rejection checks; metadata and General/XML agree. No live evidence queried." -f
    $fixtureSet.samples.Count, $ids.Count, $script:rejections)
