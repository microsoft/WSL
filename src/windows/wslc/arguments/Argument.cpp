/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Argument.cpp

Abstract:

    Implementation of the Argument class.

--*/
#include "Argument.h"
#include "Command.h"
#include "Exceptions.h"
#include "ArgumentDefinitions.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>

using namespace wsl::shared;
using namespace wsl::windows::wslc::argument;
using namespace std::literals;

namespace wsl::windows::wslc {
using namespace wsl::windows::wslc::execution;

namespace {
    using OptionalBool = std::optional<bool>;
    using OptionalLimit = std::optional<argument::Limit>;
    using OptionalString = std::optional<std::wstring>;

    struct ArgumentOverrides
    {
        OptionalString Alias;
        OptionalBool Required;
        OptionalLimit Limit;
        OptionalString Description;
    };

    Argument CreateArgument(ArgType type, ArgumentOverrides overrides)
    {
        switch (type)
        {
#define WSLC_ARG_CREATE_CASE(EnumName, Name, DefaultAlias, ArgumentKind, ConvertedType, Desc) \
    case ArgType::EnumName: \
        return Argument{ \
            type, \
            L##Name, \
            overrides.Alias.has_value() ? std::move(overrides.Alias.value()) : std::wstring{DefaultAlias}, \
            overrides.Description.has_value() ? std::move(overrides.Description.value()) : std::wstring(Desc), \
            ArgumentKind, \
            overrides.Required.value_or(Argument::DefaultRequired), \
            overrides.Limit.value_or(Argument::DefaultLimit)};

            WSLC_ARGUMENTS(WSLC_ARG_CREATE_CASE)
#undef WSLC_ARG_CREATE_CASE

        default:
            THROW_HR(E_UNEXPECTED);
        }
    }
} // namespace

Argument Argument::Create(ArgType type, OptionalBool required, OptionalLimit limit, OptionalString desc)
{
    return CreateArgument(type, {.Required = std::move(required), .Limit = std::move(limit), .Description = std::move(desc)});
}

Argument Argument::Create(ArgType type, std::wstring alias, OptionalBool required, OptionalLimit limit, OptionalString desc)
{
    return CreateArgument(
        type,
        {
            .Alias = std::move(alias),
            .Required = std::move(required),
            .Limit = std::move(limit),
            .Description = std::move(desc),
        });
}

// Retrieves the usage string of the Argument, based on its Alias and Name.
// The format is "-alias,--name" or just "--name" if no alias.
std::wstring Argument::GetUsageString() const
{
    std::wostringstream strstr;
    if (!m_alias.empty())
    {
        strstr << WSLC_CLI_ARG_ID_CHAR << m_alias << L',';
    }

    strstr << WSLC_CLI_ARG_ID_CHAR << WSLC_CLI_ARG_ID_CHAR << m_name;
    return strstr.str();
}
} // namespace wsl::windows::wslc
