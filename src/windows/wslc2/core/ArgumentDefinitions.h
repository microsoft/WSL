// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Desc, DataType, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet)
// Note: DataType is wrapped in parentheses to handle types with commas (e.g., std::vector<T>)
#define WSLC_ARGUMENTS(_) \
    _(Help,         "help",         WSLC_HELP_CHAR, Localization::WSLCCLI_HelpArgDescription(),           (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(Info,         "info",         NoAlias,        Localization::WSLCCLI_InfoArgDescription(),           (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(SessionId,    "session",      NoAlias,        Localization::WSLCCLI_SessionIdArgDescription(),      (std::wstring),               Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(Attach,       "attach",       L'a',           Localization::WSLCCLI_AttachArgDescription(),         (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(Interactive,  "interactive",  L'i',           Localization::WSLCCLI_InteractiveArgDescription(),    (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(ContainerId,  "container",    NoAlias,        Localization::WSLCCLI_ContainerIdArgDescription(),    (std::wstring),               Kind::Positional,  Visibility::Example, true,  1, Category::None, ExclusiveSet::None) \
    _(Port,         "port",         L'p',           Localization::WSLCCLI_PortArgDescription(),           (std::wstring),               Kind::Standard,    Visibility::Help,    false, 5, Category::None, ExclusiveSet::None) \
    _(ForwardArgs,  "forward_args", NoAlias,        L"Args to forward to the container start",           (std::vector<std::wstring>),  Kind::Forward,     Visibility::Example, false, 1, Category::None, ExclusiveSet::None) \
    _(ContainerIds, "containerids", NoAlias,        L"Test Arg for now",                                  (std::wstring),               Kind::Positional,  Visibility::Example, true,  1, Category::None, ExclusiveSet::None) \
    _(TestArg,      "arg",          L'a',           L"Shows Ninjacat",                                    (bool),                       Kind::Standard,    Visibility::Help,    false,  1, Category::None, ExclusiveSet::None)
