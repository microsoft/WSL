# Copyright (C) Microsoft Corporation. All rights reserved.
#requires -Version 5.1
# Native-only entry points retain the prototype's validated six-insertion decoder.

[CmdletBinding()]
param([string]$XmlPath)

function Get-ProcessSecurityLabels {
    param([bool]$Created)
    $labels = @('Event format version', 'Windows user SID', 'WSL session ID', 'Windows session ID',
        'Distribution match', 'Distribution ID', 'Distribution name', 'Capture session ID',
        'Windows received time (UTC)', 'Policy source', 'Process ID', 'Thread ID', 'Parent process ID',
        'PID namespace ID', 'User ID (UID)', 'Effective user ID (EUID)', 'Group ID (GID)', 'Effective group ID (EGID)',
        'Linux event clock (monotonic ns)', 'Linux process start clock (boottime ns)',
        'Executable', 'Working directory', 'Arguments (in order)', 'Executable path truncated',
        'Working directory truncated', 'Arguments truncated')
    if ($Created) {
        $replacements = @{
            10='Process ID (VM)'; 11='Thread ID (VM)'; 12='Creator process ID (VM)'; 13='Child PID namespace ID'
            18='Linux creation clock (monotonic ns)'; 19='Child process start clock (boottime ns)'
            20='Creator executable'; 21='Creator working directory'; 22='Creator process start clock (boottime ns)'
            23='Creator executable path truncated'; 24='Creator working directory truncated'
            25='Process ID in child PID namespace'
        }
        foreach ($index in $replacements.Keys) { $labels[$index] = $replacements[$index] }
    }
    return ,$labels
}

function Expand-ProcessSecurityFields {
    param([AllowEmptyString()][string[]]$Fields, [bool]$Created)
    $version = if ($Created) { '2' } else { '3' }
    if ($Fields.Count -ne 8 -or $Fields[0] -cne $version) { throw 'Invalid grouped process insertion layout.' }
    $labels = Get-ProcessSecurityLabels $Created
    $operation = $Fields[5] + $Fields[6]
    $split = [Math]::Min(16384, $operation.Length)
    if ($split -gt 0 -and $split -lt $operation.Length -and
        [char]::IsHighSurrogate($operation[$split - 1]) -and [char]::IsLowSurrogate($operation[$split])) { $split-- }
    if ($Fields[5].Length -ne $split) { throw 'Noncanonical process operation group split.' }
    $values = New-Object string[] 26
    $values[0] = $Fields[0]; $values[1] = $Fields[1]; $values[7] = $Fields[2]
    $linuxIndices = if ($Created) { @(10, 11, 25, 13, 14, 15, 16, 17) } else { @(10..17) }
    $operationIndices = if ($Created) { @(12, 20, 23, 21, 24) } else { @(20, 23, 21, 24, 22, 25) }
    $timingIndices = if ($Created) { @(8, 18, 19, 22, 9) } else { @(8, 18, 19, 9) }
    $groups = @(
        @{text=$Fields[3]; indices=@(6, 5, 4, 2, 3)}
        @{text=$Fields[4]; indices=$linuxIndices}
        @{text=$operation; indices=$operationIndices}
        @{text=$Fields[7]; indices=$timingIndices}
    )
    foreach ($group in $groups) {
        if ($group.text.Contains("`r")) { throw 'Process groups require LF, not CR or CRLF.' }
        $lines = [regex]::Split($group.text, "`n")
        if ($lines.Count -ne $group.indices.Count + 1 -or $lines[$lines.Count - 1] -cne '') {
            throw 'Process group requires exactly one LF-terminated line per field.'
        }
        for ($index = 0; $index -lt $group.indices.Count; $index++) {
            $target = $group.indices[$index]
            $prefix = '    ' + $labels[$target] + ': '
            if (-not $lines[$index].StartsWith($prefix, [StringComparison]::Ordinal)) {
                throw "Missing, duplicate, reordered, or misplaced process field: $($labels[$target])."
            }
            $values[$target] = $lines[$index].Substring($prefix.Length)
        }
    }
    foreach ($value in $values) {
        if ($value -match '[\x00-\x1f\x7f-\x9f]') { throw 'Unescaped control character in process scalar.' }
    }
    return ,$values
}

function Read-ProcessQuotedText {
    param([string]$Text, [int]$Maximum, [switch]$AllowEmpty)
    $token = '"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"'
    if ($Text -cnotmatch ('\A' + $token + '\z')) { throw 'Expected exactly one JSON-quoted process string.' }
    # Decode once; literal backslash-u text must not become a second Unicode escape.
    $value = [regex]::Replace($Text.Substring(1, $Text.Length - 2), '\\(["\\/bfnrt]|u[0-9a-fA-F]{4})',
        [System.Text.RegularExpressions.MatchEvaluator]{
            param($match)
            $escape = $match.Groups[1].Value
            if ($escape.StartsWith('u')) { return [string][char][Convert]::ToUInt16($escape.Substring(1), 16) }
            return @{'"'='"'; '\'='\'; '/'='/'; b="`b"; f="`f"; n="`n"; r="`r"; t="`t"}[$escape]
        })
    if ((-not $AllowEmpty -and $value.Length -eq 0) -or $value.IndexOf([char]0) -ge 0 -or
        [System.Text.UTF8Encoding]::new($false, $true).GetByteCount($value) -gt $Maximum) {
        throw 'Invalid, empty, or oversized process text.'
    }
    return $value
}

function Read-PmSecurityGroup {
    param([AllowEmptyString()][string]$Text, [string[]]$Labels)
    if ($Text.Contains("`r")) { throw 'Security groups require LF, not CR or CRLF.' }
    $lines = [regex]::Split($Text, "`n")
    if ($lines.Count -ne $Labels.Count + 1 -or $lines[$lines.Count - 1] -cne '') {
        throw 'Security group requires exactly one LF-terminated line per field.'
    }
    $values = @(
        for ($index = 0; $index -lt $Labels.Count; $index++) {
            $prefix = '    ' + $Labels[$index] + ': '
            if (-not $lines[$index].StartsWith($prefix, [StringComparison]::Ordinal)) {
                throw "Missing, duplicate, reordered, or misplaced field: $($Labels[$index])."
            }
            $value = $lines[$index].Substring($prefix.Length)
            if ($value.Length -gt 16384 -or $value -match '[\x00-\x1f\x7f-\x9f%]') {
                throw 'Unescaped control/percent character or oversized scalar.'
            }
            $value
        }
    )
    return ,$values
}

function Join-PmSecurityOperation {
    param([AllowEmptyString()][string[]]$Fields)
    if ($Fields.Count -ne 6) { throw 'Current Security payload requires exactly six insertions.' }
    $characters = 0
    foreach ($field in $Fields) {
        if ($field.Length -gt 16384) { throw 'Security insertion exceeds its UTF-16 limit.' }
        $characters += $field.Length + 1
        [void][Text.UTF8Encoding]::new($false, $true).GetByteCount($field)
    }
    if ($characters -gt 24576) { throw 'Security payload exceeds its total limit, including NUL terminators.' }
    $text = $Fields[3] + $Fields[4]
    $split = [Math]::Min(16384, $text.Length)
    if ($split -gt 0 -and $split -lt $text.Length -and
        [char]::IsHighSurrogate($text[$split - 1]) -and [char]::IsLowSurrogate($text[$split])) { $split-- }
    if ($Fields[3].Length -ne $split) { throw 'Noncanonical operation group split.' }
    return $text
}

function Read-PmSecurityNumber {
    param([string]$Text, [string]$Type)
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

function Read-PmSecurityObservation {
    param([string]$Text, [string]$Name, [string]$Type, [switch]$Required, [switch]$AllowNotApplicable)
    $pathOrArguments = $Type -cin @('path', 'argv')
    $truncated = $pathOrArguments -and $Text.EndsWith(' [truncated]', [StringComparison]::Ordinal)
    if ($truncated) { $Text = $Text.Substring(0, $Text.Length - 12) }
    $observed = [pscustomobject][ordered]@{value=$null; status='present'}
    if ($Text -ceq 'Not available') {
        if ($Required) { throw "Missing required field: $Name." }
        $observed.status = 'unavailable'
    }
    elseif ($Type -ceq 'argv' -and $Text -ceq 'Not recorded (disabled by policy)') {
        $observed.status = 'disabled_by_policy'
    }
    elseif ($AllowNotApplicable -and $Text -ceq 'Not applicable') { $observed.status = 'not_applicable' }
    elseif ($Type -ceq 'argv') {
        if ($Text -ceq 'None (0 arguments)') { $observed.value = @() }
        else {
            $token = '"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"'
            if ($Text -cnotmatch ('\A' + $token + '(?: *, *' + $token + ')*\z')) { throw 'Malformed argument list.' }
            $observed.value = @(foreach ($argument in [regex]::Matches($Text, $token)) {
                Read-ProcessQuotedText $argument.Value 4095 -AllowEmpty
            })
            $bytes = 0
            foreach ($argument in $observed.value) { $bytes += [Text.Encoding]::UTF8.GetByteCount($argument) + 1 }
            if ($bytes -gt 4096) { throw 'Arguments exceed collection limit.' }
        }
    }
    elseif ($Type -cin @('path', 'text')) { $observed.value = Read-ProcessQuotedText $Text 4096 }
    else {
        $observed.value = Read-PmSecurityNumber $Text $Type
        if ($Name -cin @('vmPid', 'vmTid', 'vmParentPid', 'vmCreatorPid', 'namespacePid', 'pidNamespace',
                'mountNamespace', 'networkNamespace', 'cgroupId', 'creatorCgroupId', 'linuxEventNs',
                'previousMountNamespace', 'operationMountNamespace', 'operationNetworkNamespace', 'operationPidNamespace') -and
            $Text -ceq '0') { throw "Zero is not a present identifier or clock: $Name." }
        if ($Name -cin @('uid', 'euid', 'gid', 'egid', 'oldUid', 'oldGid', 'newUid', 'newGid') -and
            $Text -ceq '4294967295') { throw "Credential sentinel is not a present $Name." }
        if ($Name.EndsWith('Port') -and $observed.value -gt 65535) { throw 'Port exceeds uint16 range.' }
    }
    if ($pathOrArguments) {
        if ($observed.status -cne 'present' -and $truncated) { throw 'Unavailable or disabled values cannot be truncated.' }
        $observed | Add-Member NoteProperty truncated $truncated
    }
    if ($Name -ceq 'linuxEventNs') { $observed | Add-Member NoteProperty clock 'monotonic' }
    return $observed
}

function Read-PmSecurityDistribution {
    param([string]$Text, [switch]$Wslc)
    $labels = @('Distribution name', 'Distribution ID')
    if ($Wslc) { $labels += @('WSLC session ID', 'WSLC session name', 'Container ID', 'Container name') }
    $values = Read-PmSecurityGroup $Text $labels
    if ($Wslc) {
        if ($values[0] -cne 'Not available' -or $values[1] -cne 'Not available') {
            throw 'WSLC identity must not be reported as a distribution.'
        }
        $id = $null; $name = $null
        if (($values[4] -ceq 'Not available') -ne ($values[5] -ceq 'Not available')) {
            throw 'Container ID and name must have consistent availability.'
        }
        if ($values[4] -cne 'Not available') {
            $id = Read-ProcessQuotedText $values[4] 128
            $name = Read-ProcessQuotedText $values[5] 256
        }
        return [pscustomobject]@{
            distroName=$null; distroGuid=$null
            wslcSessionId=(Read-PmSecurityNumber $values[2] 'u32')
            wslcSessionName=(Read-ProcessQuotedText $values[3] 1024)
            containerId=$id; containerName=$name
        }
    }
    if ($values[0] -ceq 'Not available' -and $values[1] -ceq 'Not available') {
        return [pscustomobject]@{distroName=$null; distroGuid=$null}
    }
    [Guid]$guid = [Guid]::Empty
    if (-not [Guid]::TryParseExact($values[1], 'B', [ref]$guid) -or $guid -eq [Guid]::Empty) {
        throw 'Invalid distribution GUID.'
    }
    return [pscustomobject]@{distroName=(Read-ProcessQuotedText $values[0] 256); distroGuid=$values[1]}
}

function Add-PmSecurityWorkload {
    param([object]$Result, [object]$Context)
    if ($Context.PSObject.Properties['wslcSessionId']) {
        foreach ($name in @('wslcSessionId', 'wslcSessionName', 'containerId', 'containerName')) {
            $Result | Add-Member NoteProperty $name $Context.$name
        }
    }
    return $Result
}

function Get-PmProcessSchema {
    param([bool]$Created)
    $credentials = @('uid|User ID (UID)|u32', 'euid|Effective user ID (EUID)|u32',
        'gid|Group ID (GID)|u32', 'egid|Effective group ID (EGID)|u32')
    if ($Created) {
        $linux = @('vmPid|Process ID (VM)|u32', 'vmTid|Thread ID (VM)|u32',
            'namespacePid|Process ID in child PID namespace|u32', 'pidNamespace|Child PID namespace ID|u32',
            'mountNamespace|Child mount namespace ID|u32', 'networkNamespace|Child network namespace ID|u32',
            'creatorCgroupId|Creator cgroup ID|u64') + $credentials
        $operation = @('vmCreatorPid|Creator process ID (VM)|u32')
        $timing = @('linuxEventNs|Linux creation clock (monotonic ns)|u64')
    }
    else {
        $linux = @('vmPid|Process ID|u32', 'vmTid|Thread ID|u32', 'vmParentPid|Parent process ID|u32',
            'namespacePid|Process ID in PID namespace|u32', 'pidNamespace|PID namespace ID|u32',
            'mountNamespace|Mount namespace ID|u32', 'networkNamespace|Network namespace ID|u32',
            'cgroupId|Cgroup ID|u64') + $credentials
        $operation = @('executable|Executable|path', 'cwd|Working directory|path', 'argv|Arguments (in order)|argv')
        $timing = @('linuxEventNs|Linux event clock (monotonic ns)|u64')
    }
    return @{linux=$linux; operation=$operation; timing=$timing}
}

function ConvertFrom-PmProcessFields {
    param([string[]]$Fields, [bool]$Created)
    $operation = Join-PmSecurityOperation $Fields
    $wslc = ($Created -and $Fields[0] -ceq '4') -or (-not $Created -and $Fields[0] -ceq '5')
    $distribution = Read-PmSecurityDistribution $Fields[1] -Wslc:$wslc
    $schema = Get-PmProcessSchema $Created
    $eventType = if ($Created) { 'ProcessCreated' } else { 'ProcessExecuted' }
    $data = [pscustomobject][ordered]@{schemaVersion=[int]$Fields[0]; eventType=$eventType; outcome='success'}
    foreach ($group in @(@{rows=$schema.linux; text=$Fields[2]}, @{rows=$schema.operation; text=$operation},
            @{rows=$schema.timing; text=$Fields[5]})) {
        $values = Read-PmSecurityGroup $group.text @($group.rows | ForEach-Object { $_.Split('|')[1] })
        for ($index = 0; $index -lt $group.rows.Count; $index++) {
            $parts = $group.rows[$index].Split('|')
            $required = $parts[0] -cin @('vmPid', 'vmTid')
            $value = Read-PmSecurityObservation $values[$index] $parts[0] $parts[2] -Required:$required
            if ($required) { $value = $value.value }
            $data | Add-Member NoteProperty $parts[0] $value
        }
    }
    if ($Created -and $data.vmPid -ne $data.vmTid) { throw 'ProcessCreated is not a new process leader.' }
    $result = [pscustomobject][ordered]@{
        eventId=$(if ($Created) {62011} else {62010}); schemaVersion=[int]$Fields[0]; eventType=$eventType
        distroGuid=$distribution.distroGuid; distroName=$distribution.distroName; data=$data
    }
    return Add-PmSecurityWorkload $result $distribution
}

function Assert-PmSecurityDescription {
    param([string[]]$Fields, [string]$Description)
    if ($Description.Length -gt 131072) { throw 'Security General rendering exceeds its character limit.' }
    $operation = Join-PmSecurityOperation $Fields
    $version = 'Event format version: ' + $Fields[0]
    if ([regex]::Matches($Description, '(?m)^[ \t]*' + [regex]::Escape($version) + "[ `t]*`r?$").Count -ne 1) {
        throw 'General does not render the current format version exactly once.'
    }
    foreach ($heading in @('Operation details', 'Linux process and observed identity', 'Windows and distribution', 'Timing and collection')) {
        if ([regex]::Matches($Description, '(?m)^[ \t]*' + [regex]::Escape($heading) + "[ `t]*`r?$").Count -ne 1) {
            throw "General lacks the $heading section."
        }
    }
    $expectedRows = @($version)
    foreach ($group in @($Fields[1], $Fields[2], $operation, $Fields[5])) {
        if (-not $Description.Contains($group)) { throw 'General does not preserve a complete ordered group.' }
        foreach ($line in $group.Split("`n")) {
            if ($line.Length -eq 0) { continue }
            $expectedRows += $line.TrimStart()
        }
    }
    $actualRows = @([regex]::Matches($Description, '(?m)^[ \t]*([^:\r\n]+: [^\r\n]*)\r?$') |
        ForEach-Object { $_.Groups[1].Value.TrimStart() })
    if ($actualRows.Count -ne $expectedRows.Count) { throw 'General has extra or missing field rows.' }
    foreach ($row in $expectedRows) {
        if (@($actualRows | Where-Object { $_ -ceq $row }).Count -ne 1) { throw 'General has duplicate or incorrect values.' }
    }
    if ($Description -match '(?m)^[ \t]*Data\s*=') { throw 'General contains a JSON data dump.' }
}

function ConvertFrom-ProcessSecurityXml {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$XmlText)

    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version Latest
    if ($XmlText.Length -gt 131072) { throw 'Process XML exceeds its character limit.' }
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

    function Read-SystemText([string]$Name) {
        $nodes = $document.SelectNodes("/e:Event/e:System/e:$Name", $ns)
        if ($nodes.Count -ne 1) { throw "Missing or duplicate Windows System.$Name." }
        return $nodes[0].InnerText
    }
    function Require-Keys($Value, [string[]]$Expected) {
        if ($Value -isnot [pscustomobject]) { throw 'Expected a JSON object.' }
        $names = @($Value.PSObject.Properties.Name)
        if ($names.Count -ne $Expected.Count) { throw 'Unexpected JSON field count.' }
        foreach ($name in $Expected) {
            if ($names -cnotcontains $name) { throw "Missing or mis-cased JSON field: $name." }
        }
    }
    function Require-Integer($Value, [uint64]$Maximum, [bool]$Nonzero = $false) {
        if ($Value -isnot [int] -and $Value -isnot [long] -and $Value -isnot [uint32] -and $Value -isnot [uint64]) {
            throw 'Expected an integer, not a string or floating-point value.'
        }
        if ($Value -lt 0 -or [uint64]$Value -gt $Maximum -or ($Nonzero -and $Value -eq 0)) {
            throw 'Integer is outside the event schema range.'
        }
    }
    function Require-Text($Value, [int]$Maximum) {
        if ($Value -isnot [string] -or $Value.IndexOf([char]0) -ge 0 -or
            [System.Text.UTF8Encoding]::new($false, $true).GetByteCount($Value) -gt $Maximum) {
            throw 'Invalid or oversized UTF-8 text.'
        }
    }
    function Read-OptionalSession([string]$Value) {
        if ($Value -ceq 'unknown') { return $null }
        [uint32]$number = 0
        if ($Value -cnotmatch '\A(0|[1-9][0-9]*)\z' -or -not [uint32]::TryParse($Value, [ref]$number)) {
            throw 'Invalid Windows session identifier.'
        }
        return $number
    }
    function Read-Json([string]$Text) {
        if ($PSVersionTable.PSVersion -ge [version]'7.5') {
            return ConvertFrom-Json -InputObject $Text -DateKind String -ErrorAction Stop
        }
        return ConvertFrom-Json -InputObject $Text -ErrorAction Stop
    }
    function Read-Decimal([string]$Text) {
        [uint64]$number = 0
        if ($Text -cnotmatch '\A(0|[1-9][0-9]*)\z' -or -not [uint64]::TryParse($Text, [ref]$number)) {
            throw 'Invalid decimal event insertion.'
        }
        return $number
    }

    if ($document.DocumentElement.LocalName -cne 'Event' -or
        $document.DocumentElement.NamespaceURI -cne $ns.LookupNamespace('e') -or
        $document.SelectNodes('/e:Event/e:System', $ns).Count -ne 1 -or
        $document.SelectNodes('/e:Event/e:EventData', $ns).Count -ne 1 -or
        $document.SelectNodes('/e:Event/e:EventData/e:Data/*', $ns).Count -ne 0) {
        throw 'Invalid process event XML structure.'
    }
    $providers = $document.SelectNodes('/e:Event/e:System/e:Provider', $ns)
    if ($providers.Count -ne 1 -or $providers[0].GetAttribute('Name') -cne 'Microsoft-Windows-WSL-SecurityAudit' -or
        (Read-SystemText 'Channel') -cne 'Security' -or (Read-SystemText 'Version') -cne '0') {
        throw 'Unexpected Security provider, channel, or Windows event version.'
    }
    $eventId = Read-SystemText 'EventID'
    $fields = @(foreach ($node in $document.SelectNodes('/e:Event/e:EventData/e:Data', $ns)) {
        foreach ($child in $node.ChildNodes) {
            if ($child.NodeType -notin @([Xml.XmlNodeType]::Text, [Xml.XmlNodeType]::CDATA,
                    [Xml.XmlNodeType]::Whitespace, [Xml.XmlNodeType]::SignificantWhitespace)) {
                throw 'Event insertions must contain only scalar text.'
            }
        }
        $node.InnerText
    })
    [int]$characters = 0
    foreach ($field in $fields) {
        if ($field.Length -gt 16384) { throw 'Security insertion exceeds its UTF-16 limit.' }
        $characters += $field.Length + 1
    }
    if ($characters -gt 24576 -or $fields.Count -eq 0 -or $fields[0] -cnotin @('1', '2', '3', '4', '5')) {
        throw 'Unsupported payload version or oversized Security payload.'
    }
    if ($fields.Count -ne 6 -or
        -not (($eventId -ceq '62010' -and $fields[0] -cin @('4', '5')) -or
              ($eventId -ceq '62011' -and $fields[0] -cin @('3', '4')))) {
        throw 'Native process events require a supported ID and its current six-insertion format.'
    }
    if ($eventId -ceq '62001') {
        if ($fields.Count -ne 27 -or $fields[0] -cne '1') { throw 'Invalid legacy event insertion count.' }
        return [pscustomobject]@{ eventId = 62001; schemaVersion = 1; eventType = 'LegacyProcess'; legacyInsertions = $fields }
    }
    if ($eventId -cnotin @('62010', '62011')) { throw 'Unsupported process event ID.' }
    $created = $eventId -ceq '62011'
    if (($created -and $fields[0] -cin @('3', '4')) -or (-not $created -and $fields[0] -cin @('4', '5'))) {
        return ConvertFrom-PmProcessFields $fields $created
    }
    if ($created -and $fields[0] -cnotin @('1', '2')) { throw 'Unsupported ProcessCreated format.' }
    $grouped = ($created -and $fields[0] -ceq '2') -or (-not $created -and $fields[0] -ceq '3')
    if ($grouped) { $fields = Expand-ProcessSecurityFields -Fields $fields -Created $created }
    $eventType = if ($created) { 'ProcessCreated' } else { 'ProcessExecuted' }
    $observedNames = if ($created) {
        @('vmCreatorPid', 'pidNamespace', 'uid', 'euid', 'gid', 'egid',
            'linuxEventNs', 'linuxStartNs', 'creatorExecutable', 'creatorCwd', 'creatorStartNs')
    } else {
        @('vmParentPid', 'pidNamespace', 'uid', 'euid', 'gid', 'egid',
            'linuxEventNs', 'linuxStartNs', 'executable', 'cwd', 'argv')
    }
    $pathNames = if ($created) { @('creatorExecutable', 'creatorCwd') } else { @('executable', 'cwd') }
    $clockNames = @('linuxEventNs', 'linuxStartNs', 'creatorStartNs')
    if ($created -or $fields[0] -cin @('2', '3')) {
        if ($fields.Count -ne 26) { throw 'Invalid readable event insertion count.' }
        foreach ($field in $fields) {
            if ($field.IndexOfAny([char[]]"`r`n`t") -ge 0) { throw 'Unescaped control character in readable event.' }
        }
        $data = [pscustomobject][ordered]@{
            schemaVersion = [int]$fields[0]; eventType = $eventType; outcome = 'success'
            vmPid = (Read-Decimal $fields[10]); vmTid = (Read-Decimal $fields[11])
        }
        $index = 12
        foreach ($name in $observedNames) {
            $text = $fields[$index++]
            $observed = [pscustomobject]@{ value = $null; status = 'present' }
            if ($text -ceq 'Not available') { $observed.status = 'unavailable' }
            elseif ($name -ceq 'argv' -and $text -ceq 'Not recorded (disabled by policy)') {
                $observed.status = 'disabled_by_policy'
            }
            elseif ($name -ceq 'argv') {
                if ($text -ceq 'None (0 arguments)') { $observed.value = @() }
                else {
                    $token = '"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"'
                    if ($text -cnotmatch ('\A' + $token + '(?: *, *' + $token + ')*\z')) { throw 'Malformed process argument list.' }
                    $observed.value = @(
                        foreach ($argument in [regex]::Matches($text, $token)) {
                            Read-ProcessQuotedText $argument.Value 4095 -AllowEmpty
                        }
                    )
                }
            }
            elseif ($name -cin $pathNames) { $observed.value = Read-ProcessQuotedText $text 4096 }
            elseif ($name -cin $clockNames) { $observed.value = $text }
            else { $observed.value = Read-Decimal $text }
            if ($name -cin $clockNames) {
                $clock = if ($name -ceq 'linuxEventNs') { 'monotonic' } else { 'boottime' }
                $observed | Add-Member NoteProperty clock $clock
            }
            if ($name -cin $pathNames -or $name -ceq 'argv') {
                $flagIndex = @{ executable = 23; cwd = 24; argv = 25; creatorExecutable = 23; creatorCwd = 24 }[$name]
                if ($fields[$flagIndex] -cnotin @('Yes', 'No')) { throw 'Invalid readable truncation flag.' }
                $observed | Add-Member NoteProperty truncated ($fields[$flagIndex] -ceq 'Yes')
            }
            $data | Add-Member NoteProperty $name $observed
        }
        if ($created) {
            $relativePid = if ($fields[25] -ceq 'Not available') {
                [pscustomobject]@{value=$null;status='unavailable'}
            } else {
                [pscustomobject]@{value=(Read-Decimal $fields[25]);status='present'}
            }
            $data | Add-Member NoteProperty namespacePid $relativePid
        }
        if ($fields[4] -ceq 'unknown') {
            if ($fields[5] -cne 'Not available' -or $fields[6] -cne 'Not available') {
                throw 'Unknown distribution lacks its explicit unavailable values.'
            }
            $fields[5] = $fields[6] = ''
        }
        # Normalize positional formats before validating their logical values.
        $fields = @($fields[0..6]) + @($eventType, $fields[7], $fields[8],
            (ConvertTo-Json -InputObject $data -Depth 8 -Compress), $fields[9])
    }
    elseif ($fields.Count -ne 12 -or $fields[7] -cne 'ProcessExecuted') {
        throw 'Unsupported process event ID or insertion layout.'
    }
    [Guid]$capture = [Guid]::Empty
    if (-not [Guid]::TryParseExact($fields[8], 'B', [ref]$capture) -or $capture -eq [Guid]::Empty) {
        throw 'Invalid capture session GUID.'
    }
    $sid = [System.Security.Principal.SecurityIdentifier]::new($fields[1])
    if ($sid.Value -cne $fields[1]) { throw 'Noncanonical Windows user SID.' }
    [DateTimeOffset]$received = [DateTimeOffset]::MinValue
    if ($fields[9] -cnotmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d{1,9})?Z$' -or
        -not [DateTimeOffset]::TryParse($fields[9], [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$received)) {
        throw 'Invalid Windows receipt time.'
    }
    if ($fields[11] -cne 'windows' -and $fields[11] -cne 'test-override') { throw 'Unknown policy source.' }
    $distroName = $null
    $distroGuid = $null
    if ($fields[4] -ceq 'matched') {
        [Guid]$guid = [Guid]::Empty
        if (-not [Guid]::TryParse($fields[5], [ref]$guid)) { throw 'Invalid distribution GUID.' }
        $distroName = Read-ProcessQuotedText $fields[6] 256
        Require-Text $distroName 256
        if ($distroName.Length -eq 0) { throw 'Empty matched distribution name.' }
        $distroGuid = $fields[5]
    }
    elseif ($fields[4] -cne 'unknown' -or $fields[5] -cne '' -or $fields[6] -cne '') {
        throw 'Inconsistent distribution attribution.'
    }
    $data = Read-Json $fields[10]
    if ($created) { $observedNames += 'namespacePid' }
    Require-Keys $data (@('schemaVersion', 'eventType', 'outcome', 'vmPid', 'vmTid') + $observedNames)
    Require-Integer $data.schemaVersion 3 $true
    if ($data.schemaVersion -ne [int]$fields[0]) { throw 'Event and data format versions differ.' }
    if ($data.eventType -cne $eventType -or $data.outcome -cne 'success') { throw 'Invalid process-event semantics.' }
    Require-Integer $data.vmPid ([uint32]::MaxValue) $true
    Require-Integer $data.vmTid ([uint32]::MaxValue) $true
    if ($created -and $data.vmPid -ne $data.vmTid) { throw 'ProcessCreated is not a new process leader.' }
    foreach ($name in $observedNames) {
        $field = $data.$name
        $pathOrArguments = $name -cin $pathNames -or $name -ceq 'argv'
        $clock = $name -cin $clockNames
        $keys = @('value', 'status')
        if ($pathOrArguments) { $keys += 'truncated' }
        if ($clock) { $keys += 'clock' }
        Require-Keys $field $keys
        if ($pathOrArguments -and $field.truncated -isnot [bool]) { throw "Invalid $name truncation flag." }
        if ($clock) {
            $expectedClock = if ($name -ceq 'linuxEventNs') { 'monotonic' } else { 'boottime' }
            if ($field.clock -cne $expectedClock) { throw "Wrong $name clock." }
        }
        if ($field.status -cne 'present') {
            if ($field.status -cne 'unavailable' -and -not ($name -ceq 'argv' -and $field.status -ceq 'disabled_by_policy')) {
                throw "Unsupported availability state for $name."
            }
            if ($null -ne $field.value -or ($pathOrArguments -and $field.truncated)) { throw "Inconsistent missing $name." }
            continue
        }
        if ($clock) {
            [uint64]$nanoseconds = 0
            if ($field.value -isnot [string] -or $field.value -cnotmatch '^[1-9][0-9]*$' -or
                -not [uint64]::TryParse($field.value, [ref]$nanoseconds)) { throw 'Invalid decimal nanosecond value.' }
        }
        elseif ($name -ceq 'argv') {
            if ($field.value -isnot [array]) { throw 'Present argv must be an array, including when empty.' }
            $bytes = 0
            foreach ($argument in $field.value) {
                Require-Text $argument 4095
                $bytes += [System.Text.Encoding]::UTF8.GetByteCount($argument) + 1
            }
            if ($bytes -gt 4096) { throw 'Arguments exceed collection limit.' }
        }
        elseif ($pathOrArguments) {
            Require-Text $field.value 4096
            if ($field.value.Length -eq 0) { throw 'Present path must not be empty.' }
        }
        else {
            $credential = $name -in @('uid', 'euid', 'gid', 'egid')
            $maximum = if ($credential) { [uint32]::MaxValue - 1L } else { [uint32]::MaxValue }
            Require-Integer $field.value $maximum (-not $credential)
        }
    }
    return [pscustomobject]@{
        eventId = [int]$eventId; schemaVersion = [int]$fields[0]; eventType = $eventType; captureSessionId = $fields[8]
        windowsUserSid = $fields[1]; wslSessionId = (Read-OptionalSession $fields[2])
        windowsSessionId = (Read-OptionalSession $fields[3]); attribution = $fields[4]
        distroGuid = $distroGuid; distroName = $distroName; receivedUtc = $fields[9]
        policySource = $fields[11]; data = $data
    }
}

function Assert-ProcessSecurityDescription {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$XmlText, [Parameter(Mandatory)][string]$Description)
    $sample = ConvertFrom-ProcessSecurityXml $XmlText
    $created = $sample.eventId -eq 62011
    if (($created -and $sample.schemaVersion -notin @(3, 4)) -or
        (-not $created -and ($sample.eventId -ne 62010 -or $sample.schemaVersion -notin @(4, 5)))) {
        throw 'Historical XML remains readable, but current General requires a six-insertion process format.'
    }
    $document = [System.Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.XmlResolver = $null
    $document.LoadXml($XmlText)
    $wireFields = @($document.SelectNodes('/*[local-name()="Event"]/*[local-name()="EventData"]/*[local-name()="Data"]') |
        ForEach-Object InnerText)
    Assert-PmSecurityDescription $wireFields $Description
}

function Assert-ProcessExecutedDescription {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$XmlText, [Parameter(Mandatory)][string]$Description)
    if ((ConvertFrom-ProcessSecurityXml $XmlText).eventId -ne 62010) { throw 'Expected ProcessExecuted.' }
    Assert-ProcessSecurityDescription -XmlText $XmlText -Description $Description
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
        if ($length -gt 131072) { throw 'Process XML exceeds its character limit.' }
        $xmlText = [string]::new($buffer, 0, $length)
    }
    finally { $stream.Dispose() }
    ConvertFrom-ProcessSecurityXml $xmlText | ConvertTo-Json -Depth 12
}
