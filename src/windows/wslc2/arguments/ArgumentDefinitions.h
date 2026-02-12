// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Kind, DataType, Desc)
// Note: DataType is wrapped in parentheses to handle types with commas (e.g., std::vector<T>)
#define WSLC_ARGUMENTS(_) \
_(All,            "all",                 L"a",              Kind::Standard,    (bool),                       L"Select all the running containers") \
_(Attach,         "attach",              L"a",              Kind::Standard,    (bool),                       Localization::WSLCCLI_AttachArgDescription()) \
_(CIDFile,        "cidfile",             NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Write the container ID to the provided path.") \
_(ContainerId,    "container-id",        NO_ALIAS,          Kind::Positional,  (std::wstring),               Localization::WSLCCLI_ContainerIdArgDescription()) \
_(DNS,            "dns",                 NO_ALIAS,          Kind::Standard,    (std::wstring),               L"IP address of the DNS nameserver in resolv.conf") \
_(DNSDomain,      "dns-domain",          NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Set the default DNS Domain") \
_(DNSOption,      "dns-option",          NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Set DNS options") \
_(DNSSearch,      "dns-search",          NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Set DNS search domains") \
_(Detach,         "detach",              L"d",              Kind::Standard,    (bool),                       L"Run a process inside the container and detach from it") \
_(Entrypoint,     "entrypoint",          NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Specifies the container init process executable") \
_(Env,            "env",                 L"e",              Kind::Standard,    (std::wstring),               L"Key=Value pairs for environment variables") \
_(EnvFile,        "env-file",            NO_ALIAS,          Kind::Standard,    (std::wstring),               L"File containing key=value pairs of env variables") \
_(Force,          "force",               L"f",              Kind::Standard,    (bool),                       L"Delete containers even if they are running") \
_(Format,         "format",              NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Output formatting (json or table) (Default:table)") \
_(ForwardArgs,    "arguments",           NO_ALIAS,          Kind::Forward,     (std::vector<std::wstring>),  L"Arguments to pass to container's init process") \
_(ProcessArgs,    "process arguments",   NO_ALIAS,          Kind::Forward,     (std::vector<std::wstring>),  L"Arguments to pass to command to be run inside the container") \
_(GroupId,        "groupid",             NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Group Id for the process") \
_(Help,           "help",                WSLC_CLI_HELP_ARG, Kind::Standard,    (bool),                       Localization::WSLCCLI_HelpArgDescription()) \
_(ImageId,        "image",               NO_ALIAS,          Kind::Positional,  (std::wstring),               L"Image name") \
_(Info,           "info",                NO_ALIAS,          Kind::Standard,    (bool),                       Localization::WSLCCLI_InfoArgDescription()) \
_(Interactive,    "interactive",         L"i",              Kind::Standard,    (bool),                       Localization::WSLCCLI_InteractiveArgDescription()) \
_(Input,          "input",               NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Path to the tar archive file containing the image.") \
_(Label,          "label",               NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Volume metadata setting") \
_(Name,           "name",                NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Name of the container") \
_(NoDNS,          "no-dns",              NO_ALIAS,          Kind::Standard,    (bool),                       L"No configuration of DNS in the container") \
_(Opt,            "opt",                 NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Set driver specific options.") \
_(Output,         "output",              L"o",              Kind::Standard,    (std::wstring),               L"Path for the saved image.") \
_(PasswordStdin,  "password-stdin",      NO_ALIAS,          Kind::Standard,    (bool),                       L"Obtain the password from stdin") \
_(Progress,       "progress",            NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Progress type (format: none|ansi) (default: ansi)") \
_(Publish,        "publish",             L"p",              Kind::Standard,    (std::wstring),               L"Publish a port from a container to host") \
_(Pull,           "pull",                NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Image pull policy (always|missing|never) (default:never)") \
_(Quiet,          "quiet",               L"q",              Kind::Standard,    (bool),                       L"Outputs the container IDs only") \
_(Registry,       "registry",            NO_ALIAS,          Kind::Positional,  (std::wstring),               L"Name of the registry server") \
_(Remove,         "remove",              L"rm",             Kind::Standard,    (bool),                       L"Remove the container after it stops") \
_(Scheme,         "scheme",              NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Use this scheme for registry connection") \
_(Server,         "server",              NO_ALIAS,          Kind::Positional,  (std::wstring),               L"Registry server name") \
_(SessionId,      "session",             NO_ALIAS,          Kind::Standard,    (std::wstring),               Localization::WSLCCLI_SessionIdArgDescription()) \
_(Signal,         "signal",              L"s",              Kind::Standard,    (std::wstring),               L"Signal to send (default: SIGKILL)") \
_(Size,           "size",                L"s",              Kind::Standard,    (std::wstring),               L"Size of the volume in bytes. Suffixes (K, M, G, T, or P)") \
_(Source,         "source",              NO_ALIAS,          Kind::Positional,  (std::wstring),               L"Current or existing image in the image-name[:tag] format") \
_(Target,         "target",              NO_ALIAS,          Kind::Positional,  (std::wstring),               L"New name for the image in the image-name[:tag] format") \
_(Time,           "time",                L"t",              Kind::Standard,    (std::wstring),               L"Time in seconds to wait before executing (default 5)") \
_(TMPFS,          "tmpfs",               NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Mount tmpfs to the container at the given path") \
_(TTY,            "tty",                 L"t",              Kind::Standard,    (bool),                       L"Open a TTY with the container process.") \
_(User,           "user",                L"u",              Kind::Standard,    (std::wstring),               L"User ID for the process (name|uid|uid:gid)") \
_(UserName,       "username",            L"u",              Kind::Standard,    (std::wstring),               L"Username for the registry connection") \
_(Verbose,        "verbose",             L"v",              Kind::Standard,    (bool),                       L"Output verbose details") \
_(Virtual,        "virtualization",      NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Expose virtualization capabilities to the container") \
_(Volume,         "volume",              NO_ALIAS,          Kind::Standard,    (std::wstring),               L"Bind mount a volume to the container") \
_(VolumeName,     "name",                NO_ALIAS,          Kind::Positional,  (std::wstring),               L"Name to give to the volume") \
_(TestArg,        "arg",                 L"a",              Kind::Standard,    (bool),                       L"Shows Ninjacat")
