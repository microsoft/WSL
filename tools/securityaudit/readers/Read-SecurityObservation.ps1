# Copyright (C) Microsoft Corporation. All rights reserved.
#requires -Version 5.1
# Acceptance is limited to the twelve native IDs below, not deferred/unsupported coverage.

[CmdletBinding()]
param([string]$XmlPath)

$savedObservationXmlPath = $XmlPath
. (Join-Path $PSScriptRoot 'Read-ProcessExecuted.ps1') -XmlPath ''
$XmlPath = $savedObservationXmlPath
Remove-Variable savedObservationXmlPath

function Get-SecurityObservationSchema {
    param([Parameter(Mandatory)][int]$EventId)

    # Independent wire mappings: do not load the writer's descriptors at runtime.
    $schemas = @{
        62020 = @('socket', 'SocketCreated', 'Socket creation', 'network')
        62021 = @('bind', 'SocketBound', 'Requested socket binding', 'network')
        62022 = @('connect', 'SocketConnect', 'Socket connection attempt', 'network')
        62023 = @('accept', 'SocketAccepted', 'Socket accept', 'network')
        62024 = @('accept4', 'SocketAccepted4', 'Socket accept4', 'network')
        62025 = @('dns-query', 'DnsQuery', 'Resolver observation', 'network')
        62030 = @('init_module', 'ModuleLoaded', 'Module image load', 'module-load')
        62031 = @('finit_module', 'ModuleLoadedFromFd', 'Module load from file descriptor', 'module-load')
        62040 = @('setuid', 'UserIdChanged', 'User identity request', 'privilege-transition')
        62041 = @('setgid', 'GroupIdChanged', 'Group identity request', 'privilege-transition')
        62042 = @('setresgid', 'GroupIdsChanged', 'Real, effective and saved group identity request', 'privilege-transition')
        62050 = @('unshare', 'NamespaceUnshare', 'Namespace unshare (policy deferred)', 'namespace-change')
        62051 = @('setns', 'NamespaceSet', 'Namespace setns (policy deferred)', 'namespace-change')
    }
    if (-not $schemas.ContainsKey($EventId)) { throw 'Unsupported security observation event ID.' }
    $schema = $schemas[$EventId]
    $common = @(
        'result|Reported return value (not a universal errno)|i64'
        'vmPid|Process ID (VM)|u32'
        'vmTid|Thread ID (VM)|u32'
        'pidNamespace|PID namespace ID|u32'
        'mountNamespace|Observed mount namespace ID|u32'
        'networkNamespace|Caller network namespace ID|u32'
        'cgroupId|Cgroup ID|u64'
        'uid|Observed user ID (UID)|u32'
        'euid|Observed effective user ID (EUID)|u32'
        'gid|Observed group ID (GID)|u32'
        'egid|Observed effective group ID (EGID)|u32'
        'linuxEventNs|Linux event clock (monotonic ns)|u64'
        'linuxStartNs|Linux process start clock (boottime ns)|u64'
        'collectionStatus|Library collection status bits|u32'
    )
    $families = @{
        network = @(
            'socketFd|Socket FD (listening FD for accept)|i32'
            'acceptedFd|Accepted socket FD|i32'
            'socketInode|Socket inode (listening socket for accept)|u64'
            'socketFamily|Socket address family number|u32'
            'socketType|Socket type number|u32'
            'requestedProtocol|Requested protocol number|i32'
            'protocol|Actual socket protocol|u32'
            'socketNetworkNamespace|Socket network namespace ID|u32'
            'localIp|Observed local IP|text'
            'localPort|Observed local port|u32'
            'remoteIp|Peer or requested destination IP|text'
            'remotePort|Peer or requested destination port|u32'
            'requestedIp|Requested bind IP|text'
            'requestedPort|Requested bind port|u32'
            'dnsName|Resolver query name|text'
            'dnsAddresses|Resolver returned addresses (in order)|addresses'
            'dnsStatus|Resolver status (0 success, 1 failure)|u32'
        )
        'module-load' = @(
            'moduleFd|Module input FD|i32'
            'imageSize|Module image size in bytes|u64'
            'flags|Module load flags|u64'
            'moduleName|Module name|text'
            'modulePath|Module path|text'
            'parameters|Module parameters|text'
            'signer|Module signer|text'
            'hash|Module hash|text'
        )
        'privilege-transition' = @(
            'requestedUid|Requested user ID|u32'
            'requestedGid|Requested group ID|u32'
            'requestedRealGid|Requested real group ID|u32'
            'requestedEffectiveGid|Requested effective group ID|u32'
            'requestedSavedGid|Requested saved group ID|u32'
            'oldUid|Previous user ID (unchanged dimension only)|u32'
            'oldGid|Previous group ID (unchanged dimension only)|u32'
            'newUid|Observed user ID after operation|u32'
            'newGid|Observed group ID after operation|u32'
            'capabilitiesBefore|Capabilities before operation|text'
            'capabilitiesAfter|Capabilities after operation|text'
        )
        'namespace-change' = @(
            'requestedFlags|Requested namespace flags|u64'
            'namespaceFd|Namespace input FD|i32'
            'namespaceTypes|Observed namespace type bits|u32'
            'previousMountNamespace|Previous mount namespace ID|u32'
            'operationMountNamespace|Operation mount namespace ID|u32'
            'operationNetworkNamespace|Operation network namespace ID|u32'
            'operationPidNamespace|Operation PID namespace ID|u32'
            'source|Mount source|text'
            'target|Mount target|text'
            'filesystemType|Filesystem type|text'
        )
    }
    $descriptors = @(
        foreach ($row in ($common + $families[$schema[3]])) {
            $parts = $row.Split('|')
            [pscustomobject]@{name = $parts[0]; label = $parts[1]; type = $parts[2]}
        }
    )
    return [pscustomobject]@{
        operation = $schema[0]; eventType = $schema[1]; heading = $schema[2]
        family = $schema[3]; fields = $descriptors
    }
}

function Read-SecurityObservationDocument {
    param([Parameter(Mandatory)][string]$XmlText)

    if ($XmlText.Length -gt 131072) { throw 'Security XML exceeds its character limit.' }
    $settings = [System.Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $settings.MaxCharactersInDocument = 131072
    $inputText = [System.IO.StringReader]::new($XmlText)
    $reader = [System.Xml.XmlReader]::Create($inputText, $settings)
    $document = [System.Xml.XmlDocument]::new()
    $document.XmlResolver = $null
    $document.PreserveWhitespace = $true
    try { $document.Load($reader) }
    finally { $reader.Dispose(); $inputText.Dispose() }
    $ns = [System.Xml.XmlNamespaceManager]::new($document.NameTable)
    $ns.AddNamespace('e', 'http://schemas.microsoft.com/win/2004/08/events/event')
    if ($document.DocumentElement.LocalName -cne 'Event' -or
        $document.DocumentElement.NamespaceURI -cne $ns.LookupNamespace('e') -or
        $document.SelectNodes('/e:Event/e:System', $ns).Count -ne 1 -or
        $document.SelectNodes('/e:Event/e:EventData', $ns).Count -ne 1) {
        throw 'Invalid Security event XML structure.'
    }
    $system = @{}
    foreach ($name in @('Provider', 'Channel', 'Version', 'EventID')) {
        $nodes = $document.SelectNodes("/e:Event/e:System/e:$name", $ns)
        if ($nodes.Count -ne 1) { throw "Missing or duplicate Windows System.$name." }
        $system[$name] = if ($name -ceq 'Provider') { $nodes[0].GetAttribute('Name') } else { $nodes[0].InnerText }
    }
    if ($system.Provider -cne 'Microsoft-Windows-WSL-SecurityAudit' -or
        $system.Channel -cne 'Security' -or $system.Version -cne '0' -or
        $system.EventID -cnotmatch '\A[1-9][0-9]{4}\z') {
        throw 'Unexpected Security provider, channel, event ID, or Windows event version.'
    }
    if ([int]$system.EventID -notin @(62010, 62011, 62020, 62021, 62022, 62023, 62024, 62030, 62031, 62040, 62041, 62042)) {
        throw 'Event ID is outside the twelve supported native acceptance IDs.'
    }
    $fields = @(
        foreach ($node in $document.SelectNodes('/e:Event/e:EventData/e:Data', $ns)) {
            foreach ($child in $node.ChildNodes) {
                if ($child.NodeType -ne [System.Xml.XmlNodeType]::Text -and
                    $child.NodeType -ne [System.Xml.XmlNodeType]::CDATA -and
                    $child.NodeType -ne [System.Xml.XmlNodeType]::Whitespace -and
                    $child.NodeType -ne [System.Xml.XmlNodeType]::SignificantWhitespace) {
                    throw 'Security insertion must contain text only.'
                }
            }
            $node.InnerText
        }
    )
    [int]$characters = 0
    foreach ($field in $fields) {
        if ($field.Length -gt 16384) { throw 'Security insertion exceeds its UTF-16 limit.' }
        $characters += $field.Length + 1
    }
    if ($characters -gt 24576 -or $fields.Count -ne 6) {
        throw 'Native Security payload must contain exactly six bounded insertions.'
    }
    return [pscustomobject]@{eventId = [int]$system.EventID; fields = $fields}
}

function Expand-SecurityObservationFields {
    param([Parameter(Mandatory)][AllowEmptyString()][string[]]$Fields, [Parameter(Mandatory)]$Schema)

    if ($Fields.Count -eq 0 -or
        -not (($Fields[0] -ceq '1' -and $Fields.Count -eq 28) -or ($Fields[0] -ceq '2' -and $Fields.Count -eq 8))) {
        throw 'Invalid grouped security observation insertion count or payload version.'
    }
    function Read-ObservationGroup([string]$Text, [string[]]$Labels) {
        if ($Text.Contains("`r")) { throw 'Security observation groups must use LF, not CR or CRLF.' }
        $lines = [regex]::Split($Text, "`n")
        if ($lines.Count -ne $Labels.Count + 1 -or $lines[$lines.Count - 1] -cne '') {
            throw 'Security observation group requires exactly one LF-terminated line per field.'
        }
        $values = @(
            for ($index = 0; $index -lt $Labels.Count; $index++) {
                $prefix = '    ' + $Labels[$index] + ': '
                if (-not $lines[$index].StartsWith($prefix, [StringComparison]::Ordinal)) {
                    throw "Missing, duplicate, reordered, or misplaced security observation field: $($Labels[$index])."
                }
                $lines[$index].Substring($prefix.Length)
            }
        )
        return ,$values
    }
    function Join-ObservationParts([string]$First, [string]$Second) {
        $text = $First + $Second
        $split = [Math]::Min(16384, $text.Length)
        if ($split -gt 0 -and $split -lt $text.Length -and
            [char]::IsHighSurrogate($text[$split - 1]) -and [char]::IsLowSurrogate($text[$split])) {
            $split--
        }
        if ($First.Length -ne $split) { throw 'Noncanonical security observation group split.' }
        return $text
    }
    $familyLabels = @($Schema.fields | Select-Object -Skip 14 | ForEach-Object { $_.label })
    if ($Fields[0] -ceq '1') {
        $details = Join-ObservationParts $Fields[26] $Fields[27]
        $values = @($Fields[0..25]) + (Read-ObservationGroup $details $familyLabels)
    }
    else {
        $windows = Read-ObservationGroup $Fields[3] @('Distribution name', 'Distribution ID', 'Distribution match',
            'WSL session ID', 'Windows session ID')
        $linux = Read-ObservationGroup $Fields[4] @($Schema.fields[1..10] | ForEach-Object { $_.label })
        $operation = Read-ObservationGroup (Join-ObservationParts $Fields[5] $Fields[6]) `
            (@('Linux operation', 'Linux outcome', $Schema.fields[0].label) + $familyLabels)
        $timing = Read-ObservationGroup $Fields[7] @('Windows received time (UTC)', $Schema.fields[11].label,
            $Schema.fields[12].label, $Schema.fields[13].label, 'Policy source')
        $values = @($Fields[0], $Fields[1], $windows[3], $windows[4], $windows[2], $windows[1], $windows[0],
            $Fields[2], $timing[0], $timing[4], $operation[0], $operation[1], $operation[2]) +
            $linux + @($timing[1], $timing[2], $timing[3]) + @($operation[3..($operation.Count - 1)])
    }
    foreach ($value in $values) {
        if ($value -match '[\x00-\x1f\x7f-\x9f]') { throw 'Unescaped control character in security observation scalar.' }
    }
    return ,$values
}

function Get-PmSecurityObservationSchema {
    param([int]$EventId)
    $schema = Get-SecurityObservationSchema $EventId
    $common = @('result', 'vmPid', 'vmTid', 'pidNamespace', 'mountNamespace', 'networkNamespace',
        'cgroupId', 'uid', 'euid', 'gid', 'egid', 'linuxEventNs')
    if ($schema.operation -ceq 'dns-query') { $common = @('result', 'vmPid', 'vmTid', 'pidNamespace', 'linuxEventNs') }
    $details = switch ($schema.operation) {
        'socket' { 'requestedProtocol' }
        'bind' { 'requestedIp'; 'requestedPort' }
        { $_ -cin @('connect', 'accept', 'accept4') } { 'localIp'; 'localPort'; 'remoteIp'; 'remotePort' }
        'dns-query' { 'dnsName' }
        'setuid' { 'oldGid'; 'newUid'; 'newGid' }
        { $_ -cin @('setgid', 'setresgid') } { 'oldUid'; 'newUid'; 'newGid' }
        { $_ -cin @('unshare', 'setns') } {
            'namespaceTypes'; 'previousMountNamespace'; 'operationMountNamespace'; 'operationNetworkNamespace'; 'operationPidNamespace'
        }
    }
    $selected = @($common) + @($details)
    $schema.fields = @($schema.fields | Where-Object { $selected -ccontains $_.name })
    return $schema
}

function ConvertFrom-PmSecurityObservationFields {
    param([int]$EventId, [string[]]$Fields)
    $schema = Get-PmSecurityObservationSchema $EventId
    $operation = Join-PmSecurityOperation $Fields
    $distribution = Read-PmSecurityDistribution $Fields[1] -Wslc:($Fields[0] -ceq '4')
    $commonNames = @('vmPid', 'vmTid', 'pidNamespace', 'mountNamespace', 'networkNamespace', 'cgroupId',
        'uid', 'euid', 'gid', 'egid')
    $linux = @($schema.fields | Where-Object { $_.name -cin $commonNames })
    $details = @($schema.fields | Where-Object { $_.name -cnotin ($commonNames + @('result', 'linuxEventNs')) })
    $result = @($schema.fields | Where-Object name -CEQ 'result')[0]
    $clock = @($schema.fields | Where-Object name -CEQ 'linuxEventNs')[0]
    $operationValues = Read-PmSecurityGroup $operation (@('Linux operation', 'Linux outcome', $result.label) +
        @($details | ForEach-Object label))
    if ($operationValues[0] -cne $schema.operation -or $operationValues[1] -cnotin @('success', 'failure', 'pending', 'unknown')) {
        throw 'Invalid operation or outcome for this event ID.'
    }
    $data = [pscustomobject][ordered]@{
        schemaVersion=[int]$Fields[0]; eventType=$schema.eventType; family=$schema.family
        operation=$schema.operation; outcome=$operationValues[1]
    }
    $texts = @{result=$operationValues[2]}
    $linuxValues = Read-PmSecurityGroup $Fields[2] @($linux | ForEach-Object label)
    for ($index = 0; $index -lt $linux.Count; $index++) { $texts[$linux[$index].name] = $linuxValues[$index] }
    for ($index = 0; $index -lt $details.Count; $index++) { $texts[$details[$index].name] = $operationValues[3 + $index] }
    $texts.linuxEventNs = (Read-PmSecurityGroup $Fields[5] @($clock.label))[0]
    foreach ($descriptor in $schema.fields) {
        $name = $descriptor.name
        $required = $name -cin @('vmPid', 'vmTid')
        # Socket family is not exported; a non-IP socket can legitimately have no IP endpoints.
        $notApplicable = $name -cin @('localIp', 'localPort', 'remoteIp', 'remotePort', 'requestedIp', 'requestedPort')
        $value = Read-PmSecurityObservation $texts[$name] $name $descriptor.type -Required:$required -AllowNotApplicable:$notApplicable
        if ($name -cin @('euid', 'egid') -and $value.status -ceq 'present' -and $value.value -eq 0) {
            throw "Ambiguous effective-ID library default is not a present $name."
        }
        $data | Add-Member NoteProperty $name $value
    }
    if ($schema.operation -ceq 'dns-query') {
        # Resolver status determines outcome independently of the reported return code.
        if ($data.outcome -cnotin @('success', 'failure')) { throw 'Invalid resolver outcome.' }
        $expectedOutcome = $data.outcome
    }
    elseif ($data.result.status -ceq 'unavailable') { $expectedOutcome = 'unknown' }
    elseif ($schema.operation -ceq 'connect' -and $data.result.value -ceq '-115') { $expectedOutcome = 'pending' }
    elseif ([int64]$data.result.value -lt 0) { $expectedOutcome = 'failure' }
    else { $expectedOutcome = 'success' }
    if ($data.outcome -cne $expectedOutcome) { throw 'Outcome disagrees with the reported result.' }
    $resultObject = [pscustomobject][ordered]@{
        eventId=$EventId; schemaVersion=[int]$Fields[0]; eventType=$schema.eventType
        distroGuid=$distribution.distroGuid; distroName=$distribution.distroName; data=$data
    }
    return Add-PmSecurityWorkload $resultObject $distribution
}

function ConvertFrom-SecurityObservationXml {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$XmlText)

    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version Latest
    $document = Read-SecurityObservationDocument $XmlText
    $eventId = $document.eventId
    $fields = $document.fields
    if ($eventId -in @(62001, 62010, 62011)) { return ConvertFrom-ProcessSecurityXml $XmlText }
    if ($fields[0] -cnotin @('3', '4')) { throw 'Native observations require current ordinary/WSLC schema versions.' }
    if ($eventId -eq 62002) {
        if ($fields.Count -ne 12 -or $fields[0] -cne '1') { throw 'Invalid legacy security insertion layout.' }
        return [pscustomobject]@{
            eventId = 62002; schemaVersion = 1; eventType = 'LegacySecurityObservation'; legacyInsertions = $fields
        }
    }
    $schema = Get-SecurityObservationSchema $eventId
    if ($fields[0] -cin @('3', '4')) { return ConvertFrom-PmSecurityObservationFields $eventId $fields }
    $fields = Expand-SecurityObservationFields -Fields $fields -Schema $schema
    if ($fields[10] -cne $schema.operation -or $fields[11] -cnotin @('success', 'failure', 'pending', 'unknown')) {
        throw 'Invalid operation or outcome for this event ID.'
    }

    function Read-ObservationJson([string]$Text) {
        $parameters = @{InputObject = $Text; ErrorAction = 'Stop'}
        if ($PSVersionTable.PSVersion -ge [version]'7.5') { $parameters.DateKind = 'String' }
        return ConvertFrom-Json @parameters
    }
    function Read-ObservationNumber([string]$Text, [string]$Type) {
        $pattern = if ($Type.StartsWith('i')) { '\A(0|-?[1-9][0-9]*)\z' } else { '\A(0|[1-9][0-9]*)\z' }
        if ($Text -cnotmatch $pattern) { throw 'Expected a canonical decimal integer.' }
        $valid = $false
        switch ($Type) {
            'u32' { [uint32]$number = 0; $valid = [uint32]::TryParse($Text, [ref]$number) }
            'i32' { [int32]$number = 0; $valid = [int32]::TryParse($Text, [ref]$number) }
            'u64' { [uint64]$number = 0; $valid = [uint64]::TryParse($Text, [ref]$number) }
            'i64' { [int64]$number = 0; $valid = [int64]::TryParse($Text, [ref]$number) }
        }
        if (-not $valid) { throw "Integer exceeds the $Type range." }
        if ($Type.EndsWith('64')) { return $Text }
        return $number
    }
    function Require-ObservationText($Value, [int]$Maximum) {
        if ($Value -isnot [string] -or $Value.Length -eq 0 -or $Value.IndexOf([char]0) -ge 0 -or
            [System.Text.UTF8Encoding]::new($false, $true).GetByteCount($Value) -gt $Maximum) {
            throw 'Invalid, empty, or oversized UTF-8 observation text.'
        }
    }
    $stringToken = '"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"'
    function Read-ObservationText([string]$Text, [int]$Maximum) {
        if ($Text -cnotmatch ('\A' + $stringToken + '\z')) { throw 'Expected exactly one JSON-quoted string.' }
        # Some JSON readers replace lone surrogates before UTF-8 validation; validate the original decoded text.
        # Decode once only: an escaped backslash followed by u0025 is literal text, not another percent escape.
        $decoded = [regex]::Replace($Text.Substring(1, $Text.Length - 2), '\\(["\\/bfnrt]|u[0-9a-fA-F]{4})',
            [System.Text.RegularExpressions.MatchEvaluator]{
                param($match)
                $escape = $match.Groups[1].Value
                if ($escape.StartsWith('u')) { return [string][char][Convert]::ToUInt16($escape.Substring(1), 16) }
                return @{'"'='"'; '\'='\'; '/'='/'; b="`b"; f="`f"; n="`n"; r="`r"; t="`t"}[$escape]
            })
        Require-ObservationText $decoded $Maximum
        $value = (Read-ObservationJson ('{"value":' + $Text + '}')).value
        if ($value -is [datetime] -or $value -is [datetimeoffset]) { $value = $decoded }
        Require-ObservationText $value $Maximum
        if ($value -cne $decoded) { throw 'JSON decoding changed the observation text.' }
        return $value
    }
    function Read-ObservationSession([string]$Text) {
        if ($Text -ceq 'unknown') { return $null }
        return Read-ObservationNumber $Text 'u32'
    }

    # Host validation is deliberately independent, with the same attribution rules as the process reader.
    [Guid]$capture = [Guid]::Empty
    if (-not [Guid]::TryParseExact($fields[7], 'B', [ref]$capture) -or $capture -eq [Guid]::Empty) {
        throw 'Invalid capture session GUID.'
    }
    $sid = [System.Security.Principal.SecurityIdentifier]::new($fields[1])
    if ($sid.Value -cne $fields[1]) { throw 'Noncanonical Windows user SID.' }
    $wslSession = Read-ObservationSession $fields[2]
    $windowsSession = Read-ObservationSession $fields[3]
    [DateTimeOffset]$received = [DateTimeOffset]::MinValue
    if ($fields[8] -cnotmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d{1,9})?Z$' -or
        -not [DateTimeOffset]::TryParse($fields[8], [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$received)) {
        throw 'Invalid Windows receipt time.'
    }
    if ($fields[9] -cnotin @('windows', 'test-override')) { throw 'Unknown policy source.' }
    $distroName = $null
    $distroGuid = $null
    if ($fields[4] -ceq 'matched') {
        [Guid]$guid = [Guid]::Empty
        if (-not [Guid]::TryParse($fields[5], [ref]$guid)) { throw 'Invalid distribution GUID.' }
        $distroName = Read-ObservationText $fields[6] 256
        $distroGuid = $fields[5]
    }
    elseif ($fields[4] -cne 'unknown' -or $fields[5] -cne 'Not available' -or $fields[6] -cne 'Not available') {
        throw 'Inconsistent distribution attribution.'
    }
    $data = [pscustomobject][ordered]@{
        schemaVersion = [int]$fields[0]; eventType = $schema.eventType; family = $schema.family
        operation = $schema.operation; outcome = $fields[11]
    }

    function Get-ObservationState([string]$Name, [bool]$Common) {
        $operation = $schema.operation
        if ($Common) {
            if ($operation -ceq 'dns-query' -and $Name -cnotin @('result', 'vmPid', 'vmTid', 'pidNamespace', 'linuxEventNs')) {
                return 'unsupported'
            }
            return 'available'
        }
        switch ($schema.family) {
            'network' {
                $dns = $operation -ceq 'dns-query'
                if ($dns -ne $Name.StartsWith('dns')) { return 'not_applicable' }
                if (-not $dns) {
                    if ($Name -cin @('protocol', 'socketNetworkNamespace')) { return 'unsupported' }
                    if (($Name -ceq 'requestedProtocol' -and $operation -cne 'socket') -or
                        ($Name -ceq 'acceptedFd' -and $operation -cnotin @('accept', 'accept4')) -or
                        ($Name -cin @('requestedIp', 'requestedPort') -and $operation -cne 'bind') -or
                        ($Name -cin @('remoteIp', 'remotePort') -and $operation -cin @('socket', 'bind')) -or
                        ($Name -ceq 'acceptedFd' -and $data.outcome -ceq 'failure') -or
                        ($Name -ceq 'socketFd' -and $operation -ceq 'socket' -and $data.outcome -ceq 'failure')) {
                        return 'not_applicable'
                    }
                    if (($Name.EndsWith('Ip') -or $Name.EndsWith('Port')) -and
                        $null -ne $data.socketFamily.value -and $data.socketFamily.value -notin @(2, 10)) {
                        return 'not_applicable'
                    }
                }
            }
            'module-load' {
                if ($Name -cin @('moduleName', 'modulePath', 'parameters', 'signer', 'hash')) { return 'unsupported' }
                if (($operation -ceq 'init_module' -and $Name -cin @('moduleFd', 'flags')) -or
                    ($operation -ceq 'finit_module' -and $Name -ceq 'imageSize')) { return 'not_applicable' }
            }
            'privilege-transition' {
                if ($Name.StartsWith('capabilities') -or ($Name -ceq 'oldUid' -and $operation -ceq 'setuid') -or
                    ($Name -ceq 'oldGid' -and $operation -cne 'setuid')) { return 'unsupported' }
                if (($Name -ceq 'requestedUid' -and $operation -cne 'setuid') -or
                    ($Name -ceq 'requestedGid' -and $operation -cne 'setgid') -or
                    ($Name.StartsWith('requested') -and $Name.EndsWith('Gid') -and
                        $Name -cne 'requestedGid' -and $operation -cne 'setresgid')) { return 'not_applicable' }
            }
            'namespace-change' {
                if ($Name -cin @('source', 'target', 'filesystemType') -or
                    ($Name -ceq 'namespaceFd' -and $operation -ceq 'unshare')) { return 'not_applicable' }
            }
        }
        return 'available'
    }

    for ($index = 0; $index -lt $schema.fields.Count; $index++) {
        $descriptor = $schema.fields[$index]
        $name = $descriptor.name
        $text = $fields[12 + $index]
        $state = Get-ObservationState $name ($index -lt 14)
        $observed = [pscustomobject][ordered]@{value = $null; status = 'present'}
        if ($state -cne 'available') {
            $literal = if ($state -ceq 'unsupported') { 'Not supported by library' } else { 'Not applicable' }
            if ($text -cne $literal) { throw "Expected $state for $name." }
            $observed.status = $state
        }
        elseif ($text -ceq 'Not available') {
            if ($name -cin @('vmPid', 'vmTid', 'dnsStatus')) { throw "Required observation is missing: $name." }
            $observed.status = 'unavailable'
        }
        else {
            if ($descriptor.type -ceq 'text') { $observed.value = Read-ObservationText $text 4096 }
            elseif ($descriptor.type -ceq 'addresses') {
                if ($text -ceq 'None (0 addresses)') { $observed.value = @() }
                else {
                    if ($text -cnotmatch ('\A' + $stringToken + '(?: *, *' + $stringToken + ')*\z')) {
                        throw 'Invalid JSON-quoted resolver address list.'
                    }
                    $tokens = [regex]::Matches($text, $stringToken)
                    if ($tokens.Count -gt 16) { throw 'Invalid resolver address count.' }
                    $addresses = @(foreach ($token in $tokens) { Read-ObservationText $token.Value 64 })
                    $observed.value = $addresses
                }
            }
            else { $observed.value = Read-ObservationNumber $text $descriptor.type }
            if ($name -cin @('vmPid', 'vmTid', 'pidNamespace', 'mountNamespace', 'networkNamespace', 'cgroupId',
                    'linuxEventNs', 'linuxStartNs', 'socketInode', 'socketNetworkNamespace', 'previousMountNamespace',
                    'operationMountNamespace', 'operationNetworkNamespace', 'operationPidNamespace') -and $text -ceq '0') {
                throw "Zero is not a present identifier or clock: $name."
            }
            if ($name -cin @('uid', 'euid', 'gid', 'egid', 'oldUid', 'oldGid', 'newUid', 'newGid') -and
                $text -ceq '4294967295') { throw "Observed credential sentinel is not a present $name." }
            if ($name -cin @('euid', 'egid') -and $text -ceq '0') {
                throw "Ambiguous effective-ID library default is not a present $name."
            }
            if ($name.EndsWith('Port') -and $observed.value -gt 65535) { throw 'Port exceeds uint16 range.' }
            if ($name -ceq 'dnsStatus' -and $observed.value -notin @(0, 1)) { throw 'Invalid resolver status.' }
        }
        if ($name -cin @('linuxEventNs', 'linuxStartNs')) {
            $clock = if ($name -ceq 'linuxEventNs') { 'monotonic' } else { 'boottime' }
            $observed | Add-Member NoteProperty clock $clock
        }
        $data | Add-Member NoteProperty $name $observed
    }
    if ($schema.operation -ceq 'dns-query') {
        $expectedOutcome = if ($data.dnsStatus.value -eq 0) { 'success' } else { 'failure' }
    }
    elseif ($data.result.status -ceq 'unavailable') { $expectedOutcome = 'unknown' }
    elseif ($schema.operation -ceq 'connect' -and $data.result.value -ceq '-115') { $expectedOutcome = 'pending' }
    elseif ([int64]$data.result.value -lt 0) { $expectedOutcome = 'failure' }
    else { $expectedOutcome = 'success' }
    if ($data.outcome -cne $expectedOutcome) { throw 'Outcome disagrees with the operation-specific reported result.' }
    return [pscustomobject][ordered]@{
        eventId = $eventId; schemaVersion = [int]$fields[0]; eventType = $schema.eventType; captureSessionId = $fields[7]
        windowsUserSid = $fields[1]; wslSessionId = $wslSession; windowsSessionId = $windowsSession
        attribution = $fields[4]; distroGuid = $distroGuid; distroName = $distroName; receivedUtc = $fields[8]
        policySource = $fields[9]; data = $data
    }
}

function Assert-SecurityObservationDescription {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$XmlText, [Parameter(Mandatory)][string]$Description)

    $sample = ConvertFrom-SecurityObservationXml $XmlText
    if ($sample.eventId -in @(62001, 62010, 62011)) {
        Assert-ProcessSecurityDescription -XmlText $XmlText -Description $Description
        return
    }
    if ($sample.eventId -eq 62002) { throw 'Legacy security events do not have the readable observation General contract.' }
    if ($sample.schemaVersion -notin @(3, 4)) { throw 'Historical General is not supported by the current six-insertion message resources.' }
    $schema = Get-SecurityObservationSchema $sample.eventId
    $wireFields = (Read-SecurityObservationDocument $XmlText).fields
    Assert-PmSecurityDescription $wireFields $Description
    if ([regex]::Matches($Description, '(?m)^[ \t]*' + [regex]::Escape($schema.heading) + "[ `t]*`r?$").Count -ne 1) {
        throw 'General lacks the operation heading.'
    }
}

if ($XmlPath) {
    $stream = [System.IO.File]::OpenText((Resolve-Path -LiteralPath $XmlPath).Path)
    try {
        $buffer = New-Object char[] 131073
        $length = 0
        while ($length -lt $buffer.Length) {
            $read = $stream.Read($buffer, $length, $buffer.Length - $length)
            if ($read -eq 0) { break }
            $length += $read
        }
        if ($length -gt 131072) { throw 'Security XML exceeds its character limit.' }
        $xmlText = [string]::new($buffer, 0, $length)
    }
    finally { $stream.Dispose() }
    ConvertFrom-SecurityObservationXml $xmlText | ConvertTo-Json -Depth 12
}
