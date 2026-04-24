/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentDefinitions.h

Abstract:

    Declaration of the available Arguments with their base properties.

--*/
#pragma once

// Here is where base argument types are defined, with their name, alias, kind, and default description.
// The description can be overridden by commands if a particular command needs a different description but otherwise the
// same argument type definition. The ArgType enum and the mapping of ArgType to data type are generated from this X-Macro, so all
// arguments must be defined here to be used in the system. The arguments defined here are the basis for all commands,
// but not all arguments need to be used by all commands, and additional properties of the arguments can be set in the command's
// GetArguments function when creating the Argument with Argument::Create.

// The Kind determines the data type:
// - Kind::Flag       -> bool
// - Kind::Value      -> std::wstring
// - Kind::Positional -> std::wstring
// - Kind::Forward    -> std::vector<std::wstring>

// No other files other than ArgumentValidation need to be changed when adding a new argument, and that is only
// if you wish to add validation for the new argument or have it use existing validation.

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Kind, Desc)
// clang-format off
#define WSLC_ARGUMENTS(_) \
_(All,            "all",                 L"a",              Kind::Flag,        Localization::WSLCCLI_AllArgDescription()) \
_(Attach,         "attach",              L"a",              Kind::Flag,        Localization::WSLCCLI_AttachArgDescription()) \
_(BuildArg,       "build-arg",           NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_BuildArgDescription()) \
_(BuildPull,      "pull",                NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_BuildPullArgDescription()) \
_(BuildTarget,    "target",              NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_BuildTargetArgDescription()) \
/*_(CIDFile,        "cidfile",             NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_CIDFileArgDescription())*/ \
_(Command,        "command",             NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_CommandArgDescription()) \
_(ContainerId,    "container-id",        NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_ContainerIdArgDescription()) \
_(Force,          "force",               L"f",              Kind::Flag,        Localization::WSLCCLI_ForceArgDescription()) \
_(Detach,         "detach",              L"d",              Kind::Flag,        Localization::WSLCCLI_DetachArgDescription()) \
_(DNS,            "dns",                 NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_DNSArgDescription()) \
/*_(DNSDomain,      "dns-domain",          NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_DNSDomainArgDescription())*/ \
_(DNSOption,      "dns-option",          NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_DNSOptionArgDescription()) \
_(DNSSearch,      "dns-search",          NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_DNSSearchArgDescription()) \
_(Domainname,     "domainname",          NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_DomainnameArgDescription()) \
_(Driver,         "driver",              L"d",              Kind::Value,       Localization::WSLCCLI_DriverArgDescription("guest")) \
_(Entrypoint,     "entrypoint",          NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_EntrypointArgDescription()) \
_(Env,            "env",                 L"e",              Kind::Value,       Localization::WSLCCLI_EnvArgDescription()) \
_(EnvFile,        "env-file",            NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_EnvFileArgDescription()) \
_(File,           "file",                L"f",              Kind::Value,       Localization::WSLCCLI_FileArgDescription()) \
_(Follow,         "follow",              L"f",              Kind::Flag,        Localization::WSLCCLI_FollowArgDescription()) \
_(Format,         "format",              NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_FormatArgDescription()) \
_(ForwardArgs,    "arguments",           NO_ALIAS,          Kind::Forward,     Localization::WSLCCLI_ForwardArgsDescription()) \
/*_(GroupId,        "groupid",             NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_GroupIdArgDescription())*/ \
_(Help,           "help",                WSLC_CLI_HELP_ARG, Kind::Flag,        Localization::WSLCCLI_HelpArgDescription()) \
_(Hostname,       "hostname",            L"h",              Kind::Value,       Localization::WSLCCLI_HostnameArgDescription()) \
_(ImageForce,     "force",               L"f",              Kind::Flag,        Localization::WSLCCLI_ImageForceArgDescription()) \
_(ImageId,        "image",               NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_ImageIdArgDescription()) \
_(ImportFile,     "file",                NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_ImportFileArgDescription()) \
_(Input,          "input",               L"i",              Kind::Value,       Localization::WSLCCLI_InputArgDescription()) \
_(Interactive,    "interactive",         L"i",              Kind::Flag,        Localization::WSLCCLI_InteractiveArgDescription()) \
_(Label,          "label",               NO_ALIAS,          Kind::Value,       L"Volume metadata setting") \
_(Name,           "name",                NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_NameArgDescription()) \
/*_(NoDNS,          "no-dns",              NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_NoDNSArgDescription())*/ \
_(NoCache,        "no-cache",            NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_NoCacheArgDescription()) \
_(NoPrune,        "no-prune",            NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_NoPruneArgDescription()) \
_(NoTrunc,        "no-trunc",            NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_NoTruncArgDescription()) \
_(ObjectId,       "object-id",           NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_ObjectIdArgDescription()) \
_(Options,        "opt",                 L"o",              Kind::Value,       Localization::WSLCCLI_OptionsArgDescription()) \
_(Output,         "output",              L"o",              Kind::Value,       Localization::WSLCCLI_OutputArgDescription()) \
_(Password,       "password",            L"p",              Kind::Value,       Localization::WSLCCLI_LoginPasswordArgDescription()) \
_(PasswordStdin,  "password-stdin",      NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_LoginPasswordStdinArgDescription()) \
_(Path,           "path",                NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_PathArgDescription()) \
/*_(Progress,       "progress",            NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_ProgressArgDescription())*/ \
_(Publish,        "publish",             L"p",              Kind::Value,       Localization::WSLCCLI_PublishArgDescription()) \
_(PublishAll,     "publish-all",         L"P",              Kind::Flag,        Localization::WSLCCLI_PublishAllArgDescription()) \
/*_(Pull,           "pull",                NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_PullArgDescription())*/ \
_(Quiet,          "quiet",               L"q",              Kind::Flag,        Localization::WSLCCLI_QuietArgDescription()) \
_(Remove,         "rm",                  NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_RemoveArgDescription()) \
/*_(Scheme,         "scheme",              NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_SchemeArgDescription())*/ \
_(Server,         "server",              NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_LoginServerArgDescription()) \
_(Session,        "session",             NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_SessionIdArgDescription()) \
_(SessionId,      "session-id",          NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_SessionIdPositionalArgDescription()) \
_(StoragePath,    "storage-path",        NO_ALIAS,          Kind::Positional,  L"Path to the session storage directory") \
_(Signal,         "signal",              L"s",              Kind::Value,       Localization::WSLCCLI_SignalArgDescription(L"SIGKILL")) \
_(Source,         "source",              NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_SourceArgDescription()) \
_(Tag,            "tag",                 L"t",              Kind::Value,       Localization::WSLCCLI_TagArgDescription()) \
_(Target,         "target",              NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_TargetArgDescription()) \
_(Time,           "time",                L"t",              Kind::Value,       Localization::WSLCCLI_TimeArgDescription()) \
_(TMPFS,          "tmpfs",               NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_TMPFSArgDescription()) \
_(TTY,            "tty",                 L"t",              Kind::Flag,        Localization::WSLCCLI_TTYArgDescription()) \
_(Type,           "type",                L"t",              Kind::Value,       Localization::WSLCCLI_TypeArgDescription()) \
_(User,           "user",                L"u",              Kind::Value,       Localization::WSLCCLI_UserArgDescription()) \
_(Username,       "username",            L"u",              Kind::Value,       Localization::WSLCCLI_LoginUsernameArgDescription()) \
_(Verbose,        "verbose",             NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_VerboseArgDescription()) \
_(Version,        "version",             L"v",              Kind::Flag,        Localization::WSLCCLI_VersionArgDescription()) \
/*_(Virtual,        "virtualization",      NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_VirtualArgDescription())*/ \
_(Volume,         "volume",              L"v",              Kind::Value,       Localization::WSLCCLI_VolumeArgDescription()) \
_(VolumeName,     "volume-name",         NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_VolumeNameArgDescription()) \
_(WorkDir,        "workdir",             L"w",              Kind::Value,       Localization::WSLCCLI_WorkingDirArgDescription()) \
// clang-format on
