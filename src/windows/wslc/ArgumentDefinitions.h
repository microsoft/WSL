// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Desc, DataType, Kind, Visibility, Required, CountLimit)
// Note: DataType is wrapped in parentheses to handle types with commas (e.g., std::vector<T>)
#define WSLC_ARGUMENTS(_) \
_(All,            "all",             L"a",           L"Select all the running containers",                         (bool),                       Kind::Standard,    Visibility::Usage,   false,  1) \
_(AllPrune,       "all",             L"a",           L"Remove all unused images not referenced by any container",  (bool),                       Kind::Standard,    Visibility::Usage,   false,  1) \
_(AllVolumes,     "all",             L"a",           L"Delete all volumes",                                        (bool),                       Kind::Standard,    Visibility::Usage,   false,  1) \
_(Attach,         "attach",          L"a",           Localization::WSLCCLI_AttachArgDescription(),                 (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(CIDFile,        "cidfile",         NO_ALIAS,        L"Write the container ID to the provided path.",              (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(ContainerId,    "container-id",    NO_ALIAS,        Localization::WSLCCLI_ContainerIdArgDescription(),            (std::wstring),               Kind::Positional,  Visibility::Help,     true,  1) \
_(ContainerIdOpt, "container-id",    NO_ALIAS,        Localization::WSLCCLI_ContainerIdArgDescription(),            (std::wstring),               Kind::Positional,  Visibility::Help,    false, 10) \
_(ContainerIdReq, "container-id",    NO_ALIAS,        Localization::WSLCCLI_ContainerIdArgDescription(),            (std::wstring),               Kind::Positional,  Visibility::Help,     true, 10) \
_(DNS,            "dns",             NO_ALIAS,        L"IP address of the DNS nameserver in resolv.conf",           (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(DNSDomain,      "dns-domain",      NO_ALIAS,        L"Set the default DNS Domain",                                (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(DNSOption,      "dns-option",      NO_ALIAS,        L"Set DNS options",                                           (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(DNSSearch,      "dns-search",      NO_ALIAS,        L"Set DNS search domains",                                    (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Detach,         "detach",          L"d",           L"Run a process inside the container and detach from it",     (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Entrypoint,     "entrypoint",      NO_ALIAS,        L"Specifies the container init process executable",           (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Env,            "env",             L"e",           L"Key=Value pairs for environment variables",                 (std::wstring),               Kind::Standard,    Visibility::Help,    false, 10) \
_(EnvFile,        "env-file",        NO_ALIAS,        L"File containing key=value pairs of env variables",          (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(ForceD,         "force",           L"f",           L"Delete containers even if they are running",                (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Format,         "format",          NO_ALIAS,        L"Output formatting (json or table) (Default:table)",         (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(ForwardArgs,    "arguments",       NO_ALIAS,        L"Arguments to pass to container's init process",             (std::vector<std::wstring>),  Kind::Forward,     Visibility::Help,    false,  1) \
_(ForwardArgsP,   "process arguments", NO_ALIAS,      L"Arguments to pass to command to be run inside the container", (std::vector<std::wstring>),Kind::Forward,     Visibility::Help,    false,  1) \
_(GroupId,        "groupid",         NO_ALIAS,        L"Group Id for the process",                                  (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Help,           "help",            WSLC_CLI_HELP_ARG, Localization::WSLCCLI_HelpArgDescription(),                (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(ImageId,        "image",           NO_ALIAS,        L"Image name",                                                (std::wstring),               Kind::Positional,  Visibility::Help,     true,  1) \
_(ImageIdReq,     "image",           NO_ALIAS,        L"Image name",                                                (std::wstring),               Kind::Positional,  Visibility::Help,     true, 10) \
_(Info,           "info",            NO_ALIAS,        Localization::WSLCCLI_InfoArgDescription(),                   (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Interactive,    "interactive",     L"i",           Localization::WSLCCLI_InteractiveArgDescription(),            (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Input,          "input",           NO_ALIAS,        L"Path to the tar archive file containing the image.",        (std::wstring),               Kind::Standard,    Visibility::Help,     true,  1) \
_(Label,          "label",           NO_ALIAS,        L"Volume metadata setting",                                   (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Name,           "name",            NO_ALIAS,        L"Name of the container",                                     (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(NoDNS,          "no-dns",          NO_ALIAS,        L"No configuration of DNS in the container",                  (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Opt,            "opt",             NO_ALIAS,        L"Set driver specific options.",                              (std::wstring),               Kind::Standard,    Visibility::Help,    false, 10) \
_(Output,         "output",          L"o",           L"Path for the saved image.",                                 (std::wstring),               Kind::Standard,    Visibility::Help,     true,  1) \
_(PasswordStdin,  "password-stdin",  NO_ALIAS,        L"Obtain the password from stdin",                            (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Progress,       "progress",        NO_ALIAS,        L"Progress type (format: none|ansi) (default: ansi)",         (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Publish,        "publish",         L"p",           L"Publish a port from a container to host",                   (std::wstring),               Kind::Standard,    Visibility::Help,    false, 10) \
_(Pull,           "pull",            NO_ALIAS,        L"Image pull policy (always|missing|never) (default:never)",  (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Quiet,          "quiet",           L"q",           L"Outputs the container IDs only",                            (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Registry,       "registry",        NO_ALIAS,        L"Name of the registry server",                               (std::wstring),               Kind::Positional,  Visibility::Usage,    true,  1) \
_(Remove,         "remove",          L"rm",          L"Remove the container after it stops",                       (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Scheme,         "scheme",          NO_ALIAS,        L"Use this scheme for registry connection",                   (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Server,         "server",          NO_ALIAS,        L"Registry server name",                                      (std::wstring),               Kind::Positional,  Visibility::Usage,    true,  1) \
_(SessionId,      "session",         NO_ALIAS,        Localization::WSLCCLI_SessionIdArgDescription(),              (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(SignalK,        "signal",          L"s",           L"Signal to send (default: SIGKILL)",                         (std::wstring),               Kind::Standard,    Visibility::Usage,   false,  1) \
_(SignalS,        "signal",          L"s",           L"Signal to send (default: SIGTERM)",                         (std::wstring),               Kind::Standard,    Visibility::Usage,   false,  1) \
_(Size,           "size",            L"s",           L"Size of the volume in bytes. Suffixes (K, M, G, T, or P)",  (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Source,         "source",          NO_ALIAS,        L"Current or existing image in the image-name[:tag] format",  (std::wstring),               Kind::Positional,  Visibility::Usage,    true,  1) \
_(Target,         "target",          NO_ALIAS,        L"New name for the image in the image-name[:tag] format",     (std::wstring),               Kind::Positional,  Visibility::Usage,    true,  1) \
_(Time,           "time",            L"t",           L"Time in seconds to wait before executing (default 5)",      (std::wstring),               Kind::Standard,    Visibility::Usage,   false,  1) \
_(TMPFS,          "tmpfs",           NO_ALIAS,        L"Mount tmpfs to the container at the given path",            (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(TTY,            "tty",             L"t",           L"Open a TTY with the container process.",                    (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(User,           "user",            L"u",           L"User ID for the process (name|uid|uid:gid)",                (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(UserName,       "username",        L"u",           L"Username for the registry connection",                      (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Verbose,        "verbose",         L"v",           L"Output verbose details",                                    (bool),                       Kind::Standard,    Visibility::Help,    false,  1) \
_(Virtual,        "virtualization",  NO_ALIAS,        L"Expose virtualization capabilities to the container",       (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(Volume,         "volume",          NO_ALIAS,        L"Bind mount a volume to the container",                      (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1) \
_(VolumeNameC,    "name",            NO_ALIAS,        L"Name to give to the volume",                                (std::wstring),               Kind::Positional,  Visibility::Help,     true,  1) \
_(VolumeNameD,    "names",           NO_ALIAS,        L"Names of the volumes to delete",                            (std::wstring),               Kind::Positional,  Visibility::Help,    false, 10) \
_(VolumeNameI,    "names",           NO_ALIAS,        L"Names of the volumes to inspect",                           (std::wstring),               Kind::Positional,  Visibility::Help,     true, 10) \
_(TestArg,        "arg",             L"a",           L"Shows Ninjacat",                                            (bool),                       Kind::Standard,    Visibility::Help,    false,  1)
