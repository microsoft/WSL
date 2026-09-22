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

Argument Argument::Create(ArgType type, ArgumentOverrides overrides)
{
    WI_ASSERT(!overrides.Name.has_value() || overrides.Name->size() >= 2);

    switch (type)
    {
#define WSLC_ARG_CREATE_CASE(EnumName, DefaultName, DefaultAlias, ArgumentKind, ConvertedType, DefaultDesc) \
    case ArgType::EnumName: \
        return Argument{ \
            type, \
            overrides.Name.has_value() ? std::move(overrides.Name.value()) : std::wstring{L##DefaultName}, \
            overrides.Alias.has_value() ? std::move(overrides.Alias.value()) : std::wstring{DefaultAlias}, \
            overrides.Desc.has_value() ? std::move(overrides.Desc.value()) : std::wstring(DefaultDesc), \
            ArgumentKind, \
            overrides.Required.value_or(Argument::DefaultRequired), \
            overrides.Limit.value_or(Argument::DefaultLimit), \
            overrides.Flags.value_or(Flags::None)};

        WSLC_ARGUMENTS(WSLC_ARG_CREATE_CASE)
#undef WSLC_ARG_CREATE_CASE

    default:
        THROW_HR(E_UNEXPECTED);
    }
}

Argument Argument::CreateGlobal(ArgType type, const Command& owner, ArgumentOverrides overrides)
{
    auto argument = Create(type, std::move(overrides));
    argument.m_globalOwner = std::cref(owner);
    return argument;
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

bool Argument::MatchesOption(std::wstring_view token) const
{
    if (!IsOption() || token.length() < 2 || token.front() != WSLC_CLI_ARG_ID_CHAR)
    {
        return false;
    }

    const bool longName = token[1] == WSLC_CLI_ARG_ID_CHAR;
    const size_t optionStart = longName ? 2 : 1;
    if (token.length() == optionStart || token[optionStart] == WSLC_CLI_ARG_ID_CHAR)
    {
        return false;
    }

    auto optionName = token.substr(optionStart);
    if (const auto separator = optionName.find_first_of(WSLC_CLI_ARG_SPLIT_CHAR); separator != std::wstring_view::npos)
    {
        optionName = optionName.substr(0, separator);
    }

    const auto& configuredName = longName ? Name() : Alias();
    return !configuredName.empty() && string::IsEqual(optionName, configuredName);
}
} // namespace wsl::windows::wslc
