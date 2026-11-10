;// Copyright (C) Microsoft Corporation. All rights reserved.

MessageIdTypeDef=DWORD
FacilityNames=(Audit=0x0)
LanguageNames=(Neutral=0x0000:MSG00000)

;// Authz requires a 16-bit ID, without severity, facility, or customer bits.
;// Keep the raw VM PID in insertion 1; insertions 2 and 3 are localized text.
MessageId=62010 SymbolicName=MSG_PROCESS_EXECUTED
Severity=Success
Facility=Audit
Language=Neutral
%2%n%3 %1
.

;// Insertion 1 remains the raw VM PID; insertion 3 is versioned JSON event data.
MessageId=62012 SymbolicName=MSG_PROCESS_EXECUTED_DETAILS
Severity=Success
Facility=Audit
Language=Neutral
%2%n%3
.
