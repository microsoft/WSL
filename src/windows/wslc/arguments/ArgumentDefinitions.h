// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
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
#define WSLC_ARGUMENTS(_) \
_(All,            "all",                 L"a",              Kind::Flag,        L"Select all the running containers") \
_(Attach,         "attach",              L"a",              Kind::Flag,        Localization::WSLCCLI_AttachArgDescription()) \
_(CIDFile,        "cidfile",             NO_ALIAS,          Kind::Value,       L"Write the container ID to the provided path.") \
_(ContainerId,    "container-id",        NO_ALIAS,          Kind::Positional,  Localization::WSLCCLI_ContainerIdArgDescription()) \
_(DNS,            "dns",                 NO_ALIAS,          Kind::Value,       L"IP address of the DNS nameserver in resolv.conf") \
_(DNSDomain,      "dns-domain",          NO_ALIAS,          Kind::Value,       L"Set the default DNS Domain") \
_(DNSOption,      "dns-option",          NO_ALIAS,          Kind::Value,       L"Set DNS options") \
_(DNSSearch,      "dns-search",          NO_ALIAS,          Kind::Value,       L"Set DNS search domains") \
_(Detach,         "detach",              L"d",              Kind::Flag,        L"Run a process inside the container and detach from it") \
_(Entrypoint,     "entrypoint",          NO_ALIAS,          Kind::Value,       L"Specifies the container init process executable") \
_(Env,            "env",                 L"e",              Kind::Value,       L"Key=Value pairs for environment variables") \
_(EnvFile,        "env-file",            NO_ALIAS,          Kind::Value,       L"File containing key=value pairs of env variables") \
_(Force,          "force",               L"f",              Kind::Flag,        L"Delete containers even if they are running") \
_(Format,         "format",              NO_ALIAS,          Kind::Value,       L"Output formatting (json or table) (Default:table)") \
_(ForwardArgs,    "arguments",           NO_ALIAS,          Kind::Forward,     L"Arguments to pass to container's init process") \
_(ProcessArgs,    "process arguments",   NO_ALIAS,          Kind::Forward,     L"Arguments to pass to command to be run inside the container") \
_(GroupId,        "groupid",             NO_ALIAS,          Kind::Value,       L"Group Id for the process") \
_(Help,           "help",                WSLC_CLI_HELP_ARG, Kind::Flag,        Localization::WSLCCLI_HelpArgDescription()) \
_(ImageId,        "image",               NO_ALIAS,          Kind::Positional,  L"Image name") \
_(Info,           "info",                NO_ALIAS,          Kind::Flag,        Localization::WSLCCLI_InfoArgDescription()) \
_(Interactive,    "interactive",         L"i",              Kind::Flag,        Localization::WSLCCLI_InteractiveArgDescription()) \
_(Input,          "input",               NO_ALIAS,          Kind::Value,       L"Path to the tar archive file containing the image.") \
_(Label,          "label",               NO_ALIAS,          Kind::Value,       L"Volume metadata setting") \
_(Name,           "name",                NO_ALIAS,          Kind::Value,       L"Name of the container") \
_(NoDNS,          "no-dns",              NO_ALIAS,          Kind::Flag,        L"No configuration of DNS in the container") \
_(Opt,            "opt",                 NO_ALIAS,          Kind::Value,       L"Set driver specific options.") \
_(Output,         "output",              L"o",              Kind::Value,       L"Path for the saved image.") \
_(PasswordStdin,  "password-stdin",      NO_ALIAS,          Kind::Flag,        L"Obtain the password from stdin") \
_(Progress,       "progress",            NO_ALIAS,          Kind::Value,       L"Progress type (format: none|ansi) (default: ansi)") \
_(Publish,        "publish",             L"p",              Kind::Value,       L"Publish a port from a container to host") \
_(Pull,           "pull",                NO_ALIAS,          Kind::Value,       L"Image pull policy (always|missing|never) (default:never)") \
_(Quiet,          "quiet",               L"q",              Kind::Flag,        L"Outputs the container IDs only") \
_(Registry,       "registry",            NO_ALIAS,          Kind::Positional,  L"Name of the registry server") \
_(Remove,         "remove",              L"rm",             Kind::Flag,        L"Remove the container after it stops") \
_(Scheme,         "scheme",              NO_ALIAS,          Kind::Value,       L"Use this scheme for registry connection") \
_(Server,         "server",              NO_ALIAS,          Kind::Positional,  L"Registry server name") \
_(SessionId,      "session",             NO_ALIAS,          Kind::Value,       Localization::WSLCCLI_SessionIdArgDescription()) \
_(Signal,         "signal",              L"s",              Kind::Value,       L"Signal to send (default: SIGKILL)") \
_(Size,           "size",                L"s",              Kind::Value,       L"Size of the volume in bytes. Suffixes (K, M, G, T, or P)") \
_(Source,         "source",              NO_ALIAS,          Kind::Positional,  L"Current or existing image in the image-name[:tag] format") \
_(Target,         "target",              NO_ALIAS,          Kind::Positional,  L"New name for the image in the image-name[:tag] format") \
_(Time,           "time",                L"t",              Kind::Value,       L"Time in seconds to wait before executing (default 5)") \
_(TMPFS,          "tmpfs",               NO_ALIAS,          Kind::Value,       L"Mount tmpfs to the container at the given path") \
_(TTY,            "tty",                 L"t",              Kind::Flag,        L"Open a TTY with the container process.") \
_(User,           "user",                L"u",              Kind::Value,       L"User ID for the process (name|uid|uid:gid)") \
_(UserName,       "username",            L"u",              Kind::Value,       L"Username for the registry connection") \
_(Verbose,        "verbose",             L"v",              Kind::Flag,        L"Output verbose details") \
_(Virtual,        "virtualization",      NO_ALIAS,          Kind::Value,       L"Expose virtualization capabilities to the container") \
_(Volume,         "volume",              NO_ALIAS,          Kind::Value,       L"Bind mount a volume to the container") \
_(VolumeName,     "name",                NO_ALIAS,          Kind::Positional,  L"Name to give to the volume")
