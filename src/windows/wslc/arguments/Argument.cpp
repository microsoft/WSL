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
    Argument CreateArgument(
        ArgType type, std::optional<std::wstring> alias, std::optional<bool> required, std::optional<argument::Limit> limit, std::optional<std::wstring> desc)
    {
        switch (type)
        {
#define WSLC_ARG_CREATE_CASE(EnumName, Name, DefaultAlias, ArgumentKind, ConvertedType, Desc) \
    case ArgType::EnumName: \
        return Argument{ \
            type, \
            L##Name, \
            alias.has_value() ? std::move(alias.value()) : std::wstring{DefaultAlias}, \
            desc.has_value() ? std::move(desc.value()) : std::wstring(Desc), \
            ArgumentKind, \
            required.value_or(Argument::DefaultRequired), \
            limit.value_or(Argument::DefaultLimit)};

            WSLC_ARGUMENTS(WSLC_ARG_CREATE_CASE)
#undef WSLC_ARG_CREATE_CASE

        default:
            THROW_HR(E_UNEXPECTED);
        }
    }
} // namespace

Argument Argument::Create(ArgType type, std::optional<bool> required, std::optional<argument::Limit> limit, std::optional<std::wstring> desc)
{
    return CreateArgument(type, std::nullopt, std::move(required), std::move(limit), std::move(desc));
}

Argument Argument::Create(ArgType type, std::wstring alias, std::optional<bool> required, std::optional<argument::Limit> limit, std::optional<std::wstring> desc)
{
    return CreateArgument(type, std::move(alias), std::move(required), std::move(limit), std::move(desc));
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
