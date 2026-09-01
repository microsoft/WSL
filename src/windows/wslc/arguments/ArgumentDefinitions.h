/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentDefinitions.h

Abstract:

    Declaration of the available Arguments with their base properties.

--*/
#pragma once

// Here is where base argument types are defined, with their default name, default alias, kind, conversion type, and
// default description. Commands can override the name, alias, required state, limit, and description. The ArgType enum,
// kind, and conversion type remain fixed. The ArgType enum and the mapping of ArgType to data type are generated from
// this X-Macro, so all arguments must be defined here to be used in the system. The arguments defined here are the basis
// for all commands, but not all arguments need to be used by all commands.

// The Kind determines the data type:
// - Kind::Flag       -> bool
// - Kind::Value      -> std::wstring
// - Kind::Positional -> std::wstring
// - Kind::Forward    -> std::vector<std::wstring>

// No other files other than ArgumentValidation need to be changed when adding a new argument, and that is only
// if you wish to add validation for the new argument or have it use existing validation.

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Kind, ConvertedType, Desc)
// ConvertedType is the type the argument's string value is converted to during validation and cached for
// execution (see ArgumentConvertedTypes.h). Use NoConversion for arguments that are not converted to a typed value.
// clang-format off
#define WSLC_ARGUMENTS(_) \
_(All,              "all",                  L"a",             Kind::Flag,       NoConversion, Localization::WSLCCLI_AllArgDescription()) \
_(Archive,          "archive",              L"a",             Kind::Flag,       NoConversion, Localization::WSLCCLI_ArchiveArgDescription()) \
_(Attach,           "attach",               L"a",             Kind::Flag,       NoConversion, Localization::WSLCCLI_AttachArgDescription()) \
_(BuildArg,         "build-arg",            NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_BuildArgDescription()) \
_(BuildLabel,       "label",                L"l",             Kind::Value,      NoConversion, Localization::WSLCCLI_LabelArgDescription()) \
_(BuildOutput,      "output",               L"o",             Kind::Value,      BuildOutput,  Localization::WSLCCLI_OutputArgDescription()) \
_(BuildPull,        "pull",                 NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_BuildPullArgDescription()) \
_(BuildTarget,      "target",               NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_BuildTargetArgDescription()) \
_(CIDFile,          "cidfile",              NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_CIDFileArgDescription()) \
_(Command,          "command",              NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_CommandArgDescription()) \
_(ContainerId,      "container-id",         NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_ContainerIdArgDescription()) \
_(Cpus,             "cpus",                 NO_ALIAS,         Kind::Value,      int64_t,      Localization::WSLCCLI_CpusArgDescription()) \
_(Force,            "force",                L"f",             Kind::Flag,       NoConversion, Localization::WSLCCLI_ForceArgDescription()) \
_(Detach,           "detach",               L"d",             Kind::Flag,       NoConversion, Localization::WSLCCLI_DetachArgDescription()) \
_(DNS,              "dns",                  NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_DNSArgDescription()) \
/*_(DNSDomain,        "dns-domain",           NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_DNSDomainArgDescription())*/ \
_(DNSOption,        "dns-option",           NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_DNSOptionArgDescription()) \
_(DNSSearch,        "dns-search",           NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_DNSSearchArgDescription()) \
_(Domainname,       "domainname",           NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_DomainnameArgDescription()) \
_(Driver,           "driver",               L"d",             Kind::Value,      NoConversion, Localization::WSLCCLI_DriverOptionDescription()) \
_(DriverOpt,        "driver-opt",           NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_DriverOptArgDescription()) \
_(Entrypoint,       "entrypoint",           NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_EntrypointArgDescription()) \
_(Env,              "env",                  L"e",             Kind::Value,      NoConversion, Localization::WSLCCLI_EnvArgDescription()) \
_(EnvFile,          "env-file",             NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_EnvFileArgDescription()) \
_(File,             "file",                 L"f",             Kind::Value,      NoConversion, Localization::WSLCCLI_FileArgDescription()) \
_(Filter,           "filter",               L"f",             Kind::Value,      KeyValuePair, Localization::WSLCCLI_FilterArgDescription()) \
_(Follow,           "follow",               L"f",             Kind::Flag,       NoConversion, Localization::WSLCCLI_FollowArgDescription()) \
_(Timestamps,       "timestamps",           L"t",             Kind::Flag,       NoConversion, Localization::WSLCCLI_TimestampsArgDescription()) \
_(Since,            "since",                NO_ALIAS,         Kind::Value,      LONGLONG,     Localization::WSLCCLI_SinceArgDescription()) \
_(Until,            "until",                NO_ALIAS,         Kind::Value,      LONGLONG,     Localization::WSLCCLI_UntilArgDescription()) \
_(Format,           "format",               NO_ALIAS,         Kind::Value,      FormatType,   Localization::WSLCCLI_FormatArgDescription()) \
_(ForwardArgs,      "arguments",            NO_ALIAS,         Kind::Forward,    NoConversion, Localization::WSLCCLI_ForwardArgsDescription()) \
_(Gateway,          "gateway",              NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_NetworkGatewayArgDescription()) \
_(Gpus,             "gpus",                 NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_GpusArgDescription()) \
/*_(GroupId,          "groupid",              NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_GroupIdArgDescription())*/ \
_(HealthCmd,        "health-cmd",           NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_HealthCmdArgDescription()) \
_(HealthInterval,   "health-interval",      NO_ALIAS,         Kind::Value,      int64_t,      Localization::WSLCCLI_HealthIntervalArgDescription()) \
_(HealthRetries,    "health-retries",       NO_ALIAS,         Kind::Value,      int,          Localization::WSLCCLI_HealthRetriesArgDescription()) \
_(HealthStartPeriod,"health-start-period",  NO_ALIAS,         Kind::Value,      int64_t,      Localization::WSLCCLI_HealthStartPeriodArgDescription()) \
_(HealthTimeout,    "health-timeout",       NO_ALIAS,         Kind::Value,      int64_t,      Localization::WSLCCLI_HealthTimeoutArgDescription()) \
_(Help,             "help",                 WSLC_CLI_HELP_ARG,Kind::Flag,       NoConversion, Localization::WSLCCLI_HelpArgDescription()) \
_(Hostname,         "hostname",             L"h",             Kind::Value,      NoConversion, Localization::WSLCCLI_HostnameArgDescription()) \
_(ImageForce,       "force",                L"f",             Kind::Flag,       NoConversion, Localization::WSLCCLI_ImageForceArgDescription()) \
_(ImageId,          "image",                NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_ImageIdArgDescription()) \
_(ImportFile,       "file",                 NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_ImportFileArgDescription()) \
_(IidFile,          "iidfile",              NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_IidFileArgDescription()) \
_(Input,            "input",                L"i",             Kind::Value,      NoConversion, Localization::WSLCCLI_InputArgDescription()) \
_(InspectFormat,    "format",               L"f",             Kind::Value,      JsonIndent,   Localization::WSLCCLI_InspectFormatArgDescription()) \
_(Interactive,      "interactive",          L"i",             Kind::Flag,       NoConversion, Localization::WSLCCLI_InteractiveArgDescription()) \
_(Internal,         "internal",             NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_NetworkInternalArgDescription()) \
_(IpAddress,        "ip",                   NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_IpAddressArgDescription()) \
_(IpRange,          "ip-range",             NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_NetworkIpRangeArgDescription()) \
_(Label,            "label",                L"l",             Kind::Value,      KeyValuePair, Localization::WSLCCLI_LabelArgDescription()) \
_(Last,             "last",                 L"n",             Kind::Value,      int,          Localization::WSLCCLI_LastArgDescription()) \
_(Latest,           "latest",               L"l",             Kind::Flag,       NoConversion, Localization::WSLCCLI_LatestArgDescription()) \
_(Link,             "link",                 NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_LinkArgDescription()) \
_(LinkLocalIp,      "link-local-ip",        NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_LinkLocalIpArgDescription()) \
_(Memory,           "memory",               L"m",             Kind::Value,      int64_t,      Localization::WSLCCLI_MemoryArgDescription()) \
_(Mount,            "mount",               NO_ALIAS,         Kind::Value,      ParsedMount,  Localization::WSLCCLI_MountArgDescription()) \
_(Name,             "name",                NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_NameArgDescription()) \
_(Network,          "network",              NO_ALIAS,         Kind::Value,      ParsedNetworkArgument, Localization::WSLCCLI_NetworkArgDescription()) \
_(NetworkAlias,     "network-alias",        NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_NetworkAliasArgDescription()) \
_(NetworkName,      "network-name",         NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_NetworkNameArgDescription()) \
/*_(NoDNS,            "no-dns",               NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_NoDNSArgDescription())*/ \
_(NoCache,          "no-cache",             NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_NoCacheArgDescription()) \
_(NoColor,          "no-color",             NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_NoColorArgDescription()) \
_(NoHealthcheck,    "no-healthcheck",       NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_NoHealthcheckArgDescription()) \
_(NoPrune,          "no-prune",             NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_NoPruneArgDescription()) \
_(NoTrunc,          "no-trunc",             NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_NoTruncArgDescription()) \
_(ObjectId,         "object-id",            NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_ObjectIdArgDescription()) \
_(Options,          "opt",                  L"o",             Kind::Value,      KeyValuePair, Localization::WSLCCLI_OptionsArgDescription()) \
_(Output,           "output",               L"o",             Kind::Value,      NoConversion, Localization::WSLCCLI_OutputArgDescription()) \
_(Password,         "password",             L"p",             Kind::Value,      NoConversion, Localization::WSLCCLI_LoginPasswordArgDescription()) \
_(PasswordStdin,    "password-stdin",       NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_LoginPasswordStdinArgDescription()) \
_(Path,             "path",                 NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_PathArgDescription()) \
_(Progress,         "progress",             NO_ALIAS,         Kind::Value,      ProgressMode, Localization::WSLCCLI_ProgressArgDescription()) \
_(Publish,          "publish",              L"p",             Kind::Value,      NoConversion, Localization::WSLCCLI_PublishArgDescription()) \
_(PublishAll,       "publish-all",          L"P",             Kind::Flag,       NoConversion, Localization::WSLCCLI_PublishAllArgDescription()) \
_(Pull,             "pull",                 NO_ALIAS,         Kind::Value,      PullPolicy,   Localization::WSLCCLI_PullArgDescription()) \
_(Quiet,            "quiet",                L"q",             Kind::Flag,       NoConversion, Localization::WSLCCLI_QuietArgDescription()) \
_(Remove,           "rm",                   NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_RemoveArgDescription()) \
/*_(Scheme,           "scheme",               NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_SchemeArgDescription())*/ \
_(Secret,           "secret",               NO_ALIAS,         Kind::Value,      BuildSecret,  Localization::WSLCCLI_SecretArgDescription()) \
_(Server,           "server",               NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_LoginServerArgDescription()) \
_(Session,          "session",              NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_SessionIdArgDescription()) \
_(ShmSize,          "shm-size",             NO_ALIAS,         Kind::Value,      int64_t,      Localization::WSLCCLI_ShmSizeArgDescription()) \
_(StoragePath,      "storage-path",         NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_StoragePathArgDescription()) \
_(Signal,           "signal",               L"s",             Kind::Value,      WSLCSignal,   Localization::WSLCCLI_SignalArgDescription()) \
_(Source,           "source",               NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_SourceArgDescription()) \
_(StopSignal,       "stop-signal",          NO_ALIAS,         Kind::Value,      WSLCSignal,   Localization::WSLCCLI_StopSignalArgDescription()) \
_(StopTimeout,      "stop-timeout",         NO_ALIAS,         Kind::Value,      int,          Localization::WSLCCLI_StopTimeoutArgDescription()) \
_(Subnet,           "subnet",               NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_NetworkSubnetArgDescription()) \
_(Tail,             "tail",                 L"n",             Kind::Value,      ULONGLONG,    Localization::WSLCCLI_TailArgDescription()) \
_(Tag,              "tag",                  L"t",             Kind::Value,      NoConversion, Localization::WSLCCLI_TagArgDescription()) \
_(Target,           "target",               NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_TargetArgDescription()) \
_(Time,             "time",                 L"t",             Kind::Value,      LONG,         Localization::WSLCCLI_TimeArgDescription()) \
_(TMPFS,            "tmpfs",                NO_ALIAS,         Kind::Value,      ParsedMount,  Localization::WSLCCLI_TMPFSArgDescription()) \
_(TTY,              "tty",                  L"t",             Kind::Flag,       NoConversion, Localization::WSLCCLI_TTYArgDescription()) \
_(Type,             "type",                 L"t",             Kind::Value,      InspectType,  Localization::WSLCCLI_TypeArgDescription()) \
_(Ulimit,           "ulimit",               NO_ALIAS,         Kind::Value,      UlimitValue,  Localization::WSLCCLI_UlimitArgDescription()) \
_(User,             "user",                 L"u",             Kind::Value,      NoConversion, Localization::WSLCCLI_UserArgDescription()) \
_(Username,         "username",             L"u",             Kind::Value,      NoConversion, Localization::WSLCCLI_LoginUsernameArgDescription()) \
_(Verbose,          "verbose",              NO_ALIAS,         Kind::Flag,       NoConversion, Localization::WSLCCLI_VerboseArgDescription()) \
_(Version,          "version",              L"v",             Kind::Flag,       NoConversion, Localization::WSLCCLI_VersionArgDescription()) \
/*_(Virtual,          "virtualization",       NO_ALIAS,         Kind::Value,      NoConversion, Localization::WSLCCLI_VirtualArgDescription())*/ \
_(Volume,           "volume",               L"v",             Kind::Value,      ParsedMount,  Localization::WSLCCLI_VolumeArgDescription()) \
_(VolumeName,       "volume-name",          NO_ALIAS,         Kind::Positional, NoConversion, Localization::WSLCCLI_VolumeNameArgDescription()) \
_(Volumes,          "volumes",              L"v",             Kind::Flag,       NoConversion, Localization::WSLCCLI_RemoveVolumesArgDescription()) \
_(WorkDir,          "workdir",              L"w",             Kind::Value,      NoConversion, Localization::WSLCCLI_WorkingDirArgDescription()) \
// clang-format on
