// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include "LocalizeMacros.h"

// X-Macro for defining all arguments in one place
// Format: ARGUMENT(EnumName, Name, Alias, Description, DataType, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet)
// Note: DataType is wrapped in parentheses to handle types with commas (e.g., std::vector<T>)
#define WSLC_ARGUMENTS(_) \
    _(Help,         "help",         WSLC_HELP_CHAR, WSLC_LOC_ARG(Help),         (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(Info,         "info",         L'i',           WSLC_LOC_ARG(Info),         (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(SessionId,    "session",      L's',           WSLC_LOC_ARG(SessionId),    (std::wstring),               Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(Attach,       "attach",       L'a',           WSLC_LOC_ARG(Attach),       (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(Interactive,  "interactive",  L'i',           WSLC_LOC_ARG(Interactive),  (bool),                       Kind::Standard,    Visibility::Help,    false, 1, Category::None, ExclusiveSet::None) \
    _(ContainerId,  "containerid",  NoAlias,        WSLC_LOC_ARG(ContainerId),  (std::wstring),               Kind::Positional,  Visibility::Example, true,  1, Category::None, ExclusiveSet::None) \
    _(ContainerIds, "containerids", NoAlias,        WSLC_LOC_ARG(ContainerIds), (std::vector<std::wstring>),  Kind::Positional,  Visibility::Example, true,  1, Category::None, ExclusiveSet::None) \
    _(TestArg,      "arg",          L'a',           L"Display ninjacat",        (bool),                       Kind::Standard,    Visibility::Help,    true,  1, Category::None, ExclusiveSet::None)