;// Copyright (C) Microsoft Corporation. All rights reserved.

MessageIdTypedef=DWORD
LanguageNames=(English=0x409:MSG00409)

MessageId=62001
SymbolicName=WSL_SECURITY_PROCESS
Language=English
WSL Linux process Security audit (experimental client Authz backend).%n%n
SchemaVersion=%1%n
WindowsUserSid=%2%n
WslSessionId=%3%n
WindowsSessionId=%4%n
Attribution=%5%n
DistroGuid=%6%n
DistroName=%7%n
Pid=%8%n
Tid=%9%n
Ppid=%10%n
Executable=%11%n
Cwd=%12%n
Uid=%13%n
Euid=%14%n
Gid=%15%n
Egid=%16%n
PidNamespace=%17%n
LinuxEventNs=%18%n
LinuxStartNs=%19%n
ReceivedUtc=%20%n
CommandLineIncluded=%21%n
Argv=%22%n
CmdlineTruncated=%23%n
ExecutableMissing=%24%n
CwdMissing=%25%n
CmdlineMissing=%26%n
PolicySource=%27
.

MessageId=62002
SymbolicName=WSL_SECURITY_EVENT
Language=English
WSL Linux security observation (experimental client Authz backend).%n
AuditSuccess describes recording this observation, not the Linux operation outcome.%n%n
SchemaVersion=%1%n
WindowsUserSid=%2%n
WslSessionId=%3%n
WindowsSessionId=%4%n
Attribution=%5%n
DistroGuid=%6%n
DistroName=%7%n
Family=%8%n
Operation=%9%n
ReceivedUtc=%10%n
Data=%11%n
PolicySource=%12
.

MessageId=62010
SymbolicName=WSL_PROCESS_EXECUTED
Language=English
WSL Linux ProcessExecuted Security audit (experimental event 62010).%n
A Linux program was executed successfully.%n
This records program execution, not process exit or creation of a new process.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Process and thread IDs are VM IDs; credentials are observed after execution.%n
Not available means the value was not collected; it does not mean zero.%n
Quoted text uses backslash escapes. Arguments are separated by commas, not reconstructed as a shell command.%n
The optional [truncated] suffix describes incomplete recorded text, not an additional field.%n
The Linux event clock is monotonic, not UTC.
.

MessageId=62011
SymbolicName=WSL_PROCESS_CREATED
Language=English
WSL Linux ProcessCreated Security audit (experimental event 62011).%n
A new Linux user process was created.%n
This is not a program-execution or process-exit event.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Credentials describe the child at creation; its creator may differ from its Linux parent.%n
Not available means the value was not collected; it does not mean zero.%n
The cgroup identity describes the creator, not necessarily the child.%n
Creation API, clone flags, creator thread ID, paths and command-line arguments are not recorded.%n
Quoted text uses backslash escapes. The Linux creation clock is monotonic, not UTC.
.

MessageId=62020
SymbolicName=WSL_SOCKET_CREATED
Language=English
WSL Linux SocketCreated Security audit (experimental event 62020).%n
Socket creation%n
AuditSuccess means this observation was recorded, not that the Linux operation succeeded.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Requested protocol is not the actual socket protocol. No program path or arguments are collected.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Quoted text uses backslash escapes. Fields without a supported source are not recorded.
.

MessageId=62021
SymbolicName=WSL_SOCKET_BOUND
Language=English
WSL Linux SocketBound Security audit (experimental event 62021).%n
Requested socket binding%n
AuditSuccess means this observation was recorded, not that the Linux operation succeeded.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
The requested bind endpoint is not proof of the assigned endpoint; requested port zero is valid.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Quoted text uses backslash escapes. Fields without a supported source are not recorded.
.

MessageId=62022
SymbolicName=WSL_SOCKET_CONNECT
Language=English
WSL Linux SocketConnect Security audit (experimental event 62022).%n
Socket connection attempt%n
AuditSuccess means this observation was recorded, not that the Linux operation succeeded.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Pending (-115) is not connection completion or final failure. No later completion or DNS correlation is claimed.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Quoted text uses backslash escapes. Fields without a supported source are not recorded.
.

MessageId=62023
SymbolicName=WSL_SOCKET_ACCEPTED
Language=English
WSL Linux SocketAccepted Security audit (experimental event 62023).%n
Socket accept%n
AuditSuccess means this observation was recorded, not that the Linux operation succeeded.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Missing peer data is not fabricated.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Quoted text uses backslash escapes. Fields without a supported source are not recorded.
.

MessageId=62024
SymbolicName=WSL_SOCKET_ACCEPTED4
Language=English
WSL Linux SocketAccepted4 Security audit (experimental event 62024).%n
Socket accept4%n
AuditSuccess means this observation was recorded, not that the Linux operation succeeded.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Missing peer data is not fabricated. Accept4 flags are not recorded.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Quoted text uses backslash escapes. Fields without a supported source are not recorded.
.

MessageId=62025
SymbolicName=WSL_DNS_QUERY
Language=English
WSL Linux DnsQuery Security audit (experimental event 62025).%n
Resolver observation%n
AuditSuccess means this observation was recorded, not that the resolver succeeded.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Resolver status determines outcome; the return value is not necessarily Linux errno.%n
No connection correlation, resolver API identity, complete-answer or truncation guarantee is available.%n
The VM-root collector does not currently capture resolver calls using a different distribution libc.%n
Quoted text uses backslash escapes. Fields without a supported source are not recorded.
.

MessageId=62030
SymbolicName=WSL_MODULE_LOADED
Language=English
WSL Linux ModuleLoaded Security audit (experimental event 62030).%n
Module image load%n
AuditSuccess describes recording. Linux outcome describes the init_module result.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
No pointer is interpreted as a module name, path or parameter string. Current policy permits success only.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Module identity, path, parameters, signer and hash are not recorded. The Linux event clock is monotonic, not UTC.
.

MessageId=62031
SymbolicName=WSL_MODULE_LOADED_FROM_FD
Language=English
WSL Linux ModuleLoadedFromFd Security audit (experimental event 62031).%n
Module load from file descriptor%n
AuditSuccess describes recording. Linux outcome describes the finit_module result.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
No verified module path or signature is available. Current policy permits success only.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Module identity, path, parameters, signer and hash are not recorded. The Linux event clock is monotonic, not UTC.
.

MessageId=62040
SymbolicName=WSL_USER_ID_CHANGED
Language=English
WSL Linux UserIdChanged Security audit (experimental event 62040).%n
User identity request%n
A successful request need not change the numerical ID. Current policy permits success only.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Only the unchanged group dimension can supply a previous ID; the previous user ID is unsupported.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
No complete credential or capability snapshot is claimed.
.

MessageId=62041
SymbolicName=WSL_GROUP_ID_CHANGED
Language=English
WSL Linux GroupIdChanged Security audit (experimental event 62041).%n
Group identity request%n
A successful request need not change the numerical ID. Current policy permits success only.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Only the unchanged user dimension can supply a previous ID; the previous group ID is unsupported.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
No complete credential or capability snapshot is claimed.
.

MessageId=62042
SymbolicName=WSL_GROUP_IDS_CHANGED
Language=English
WSL Linux GroupIdsChanged Security audit (experimental event 62042).%n
Real, effective and saved group identity request%n
A successful request need not change the numerical IDs. Current policy permits success only.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Only the unchanged user dimension can supply a previous ID. No saved-ID or capability snapshot is claimed.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Requested group-ID arguments are not recorded.
.

MessageId=62050
SymbolicName=WSL_NAMESPACE_UNSHARE
Language=English
WSL Linux NamespaceUnshare Security audit (experimental event 62050; schema only, emission disabled).%n
Namespace unshare (policy deferred)%n
This message definition does not enable collection or establish an audit-policy mapping.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Namespace observations are partial, not a mount operation or a complete before/after namespace snapshot.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Fields without a supported source are not recorded.
.

MessageId=62051
SymbolicName=WSL_NAMESPACE_SET
Language=English
WSL Linux NamespaceSet Security audit (experimental event 62051; schema only, emission disabled).%n
Namespace setns (policy deferred)%n
This message definition does not enable collection or establish an audit-policy mapping.%n%n
Operation details%n
%4%5%n
Linux process and observed identity%n
%3%n
Windows and distribution%n
%2%n
Timing and collection%n
%6%n
    Event format version: %1%n%n
Namespace observations are partial, not a mount operation or a complete before/after namespace snapshot.%n
Not available includes uncertain effective-ID zero readings; it does not mean root.%n
Fields without a supported source are not recorded.
.
