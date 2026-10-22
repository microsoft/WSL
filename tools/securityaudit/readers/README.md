# Native Security event readers

These standalone PowerShell 5.1+ readers accept **only**
`Microsoft-Windows-WSL-SecurityAudit`, channel `Security`, Windows event version
`0`, and the current six-insertion formats. They reuse the prototype's validated
versioned field decoding, canonical number/string handling, UTF-8/UTF-16 bounds,
identity rules and General-description checks. There is no dependency on the
prototype repository at runtime.

Acceptance is limited to these **12 IDs**:

| IDs | Current ordinary / WSLC payload versions |
| --- | --- |
| 62010 — process execution | 4 / 5 |
| 62011 — process creation | 3 / 4 |
| 62020–62024 — socket, bind, connect, accept, accept4 | 3 / 4 |
| 62030–62031 — init_module, finit_module | 3 / 4 |
| 62040–62042 — setuid, setgid, setresgid | 3 / 4 |

DNS (62025), deferred namespace IDs (62050/62051), historical prototype IDs,
capset, mount and time changes are **not acceptance successes**. XML entry points
reject IDs outside the table. Legacy/Test/Verify provider names and native-name
suffixes are also rejected; the legacy prototype scripts are not changed.

## Entry points

The unified reader handles all 12 IDs, including both process IDs:

```powershell
. .\tools\securityaudit\readers\Read-SecurityObservation.ps1 -XmlPath ''

# Supply the original event XML and its actual FormatDescription() result.
$parsed = ConvertFrom-SecurityObservationXml -XmlText $originalXml
Assert-SecurityObservationDescription -XmlText $originalXml -Description $actualGeneral
```

Process-specific entry points, available from the same dot-source operation:

```powershell
ConvertFrom-ProcessSecurityXml -XmlText $originalXml                  # 62010 or 62011
Assert-ProcessSecurityDescription -XmlText $originalXml -Description $actualGeneral
Assert-ProcessExecutedDescription -XmlText $originalXml -Description $actualGeneral # 62010 only
```

For an exported XML file, either script can run directly; the observation script
is the unified entry point:

```powershell
.\tools\securityaudit\readers\Read-SecurityObservation.ps1 -XmlPath .\evidence\event.xml
.\tools\securityaudit\readers\Read-ProcessExecuted.ps1 -XmlPath .\evidence\process.xml
```

File mode outputs decoded JSON, but does not validate General unless an assertion
function is separately given the actual description. General assertions return
no value on success and throw on mismatch. Optional System metadata such as
EventRecordID, TimeCreated, Computer, Execution and Security is compatible with
decoding but is not promoted into Linux workload identity. Preserve the original
XML/metadata alongside the parsed payload as evidence.

These are format validators, not event-origin authenticators. They do not query
Security, register sources, change policy, start WSL, install software, or rewrite
input XML/provider names. **Never alter a live provider name to make validation
pass.**

## Synthetic regression fixtures — not live proof

```powershell
powershell.exe -NoProfile -File .\tools\securityaudit\readers\Test-NativeSecurityReaders.ps1
pwsh.exe -NoProfile -File .\tools\securityaudit\readers\Test-NativeSecurityReaders.ps1
```

The checked-in fixture JSON is explicitly marked `synthetic-not-live`. Its
six-field payload and compiled General snapshots came from the existing pure
writer fixture generator; native XML was newly constructed from those fields,
not obtained by relabeling live events. The prototype and native message resource
sources were compared after line-ending normalization and matched at generation.
The input snapshot and generator are not runtime/test dependencies.

The 32 fixtures cover all 12 IDs in ordinary and WSLC form, plus unknown,
escaped, literal-sentinel and UTF-8-boundary identities. Tests compare exact
typed decoding against the captured writer projection; validate General with
and without optional XML metadata; and reject other providers, malformed
metadata/insertions, unsupported schemas/IDs, cross-workload identity confusion,
oversized XML/payload/General, overflowing integers and DTDs.

Fixture success proves reader/contract compatibility only. It does **not** prove
that a collector emitted an event, an Authz write succeeded, an installed source
renders correctly, or any particular host has live coverage.
