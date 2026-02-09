// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Desc, DataType, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet)
// Note: DataType is wrapped in parentheses to handle types with commas (e.g., std::vector<T>)
#define WSLC_ARGUMENTS(_) \
_(Help,         "help",            WSLC_HELP_CHAR, Localization::WSLCCLI_HelpArgDescription(),                   (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Info,         "info",            NoAlias,        Localization::WSLCCLI_InfoArgDescription(),                   (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(SessionId,    "session",         NoAlias,        Localization::WSLCCLI_SessionIdArgDescription(),              (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Attach,       "attach",          L'a',           Localization::WSLCCLI_AttachArgDescription(),                 (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Interactive,  "interactive",     L'i',           Localization::WSLCCLI_InteractiveArgDescription(),            (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(ContainerId,  "container",       NoAlias,        Localization::WSLCCLI_ContainerIdArgDescription(),            (std::wstring),               Kind::Positional,  Visibility::Example, true,   1, Category::None, ExclusiveSet::None) \
_(Publish,      "publish",         L'p',           L"Publish a port from a container to host",                   (std::wstring),               Kind::Standard,    Visibility::Help,    false, 10, Category::None, ExclusiveSet::None) \
_(ForwardArgs,  "arguments",       NoAlias,        L"Arguments to pass to container's init process",             (std::vector<std::wstring>),  Kind::Forward,     Visibility::Example, false,  1, Category::None, ExclusiveSet::None) \
_(ImageId,      "image",           NoAlias,        L"Image name",                                                (std::wstring),               Kind::Positional,  Visibility::Example, true,   1, Category::None, ExclusiveSet::None) \
_(CIDFile,      "cidfile",         NoAlias,        L"Write the container ID to the provided path.",              (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(DNS,          "dns",             NoAlias,        L"IP address of the DNS nameserver in resolv.conf",           (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(DNSDomain,    "dns-domain",      NoAlias,        L"Set the default DNS Domain",                                (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(DNSOption,    "dns-option",      NoAlias,        L"Set DNS options",                                           (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(DNSSearch,    "dns-search",      NoAlias,        L"Set DNS search domains",                                    (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Entrypoint,   "entrypoint",      NoAlias,        L"Specifies the container init process executable",           (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Env,          "env",             L'e',           L"Key=Value pairs for environment variables",                 (std::wstring),               Kind::Standard,    Visibility::Help,    false, 10, Category::None, ExclusiveSet::None) \
_(EnvFile,      "env-file",        NoAlias,        L"File containing key=value pairs of env variables",          (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(GroupId,      "groupid",         NoAlias,        L"Group Id for the process",                                  (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Name,         "name",            NoAlias,        L"Name of the container",                                     (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(NoDNS,        "no-dns",          NoAlias,        L"No configuration of DNS in the container",                  (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Progress,     "progress",        NoAlias,        L"Progress type (format: none|ansi) (default: ansi)",         (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Remove,       "remove",          L'r',           L"Remove the container after it stops",                       (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Pull,         "pull",            NoAlias,        L"Image pull policy (always|missing|never) (default:never)",  (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Scheme,       "scheme",          NoAlias,        L"Use this scheme for registry connection",                   (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(TMPFS,        "tmpfs",           NoAlias,        L"Mount tmpfs to the container at the given path",            (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(TTY,          "tty",             L't',           L"Open a TTY with the container process.",                    (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(User,         "user",            L'u',           L"User ID for the process (name|uid|uid:gid)",                (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Volume,       "volume",          NoAlias,        L"Bind mount a volume to the container",                      (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(Virtual,      "virtualization",  NoAlias,        L"Expose virtualization capabilities to the container",       (std::wstring),               Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None) \
_(TestArg,      "arg",             L'a',           L"Shows Ninjacat",                                            (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None)
