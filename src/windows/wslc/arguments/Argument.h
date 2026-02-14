/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Argument.h

Abstract:

    Declaration of the Argument class for command-line argument handling.

--*/
#pragma once
#include "ArgumentTypes.h"

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#define WSLC_CLI_ARG_ID_CHAR L'-'
#define WSLC_CLI_ARG_ID_STRING L"-"
#define WSLC_CLI_ARG_SPLIT_CHAR L'='
#define WSLC_CLI_HELP_ARG L"?"
#define WSLC_CLI_HELP_ARG_STRING WSLC_CLI_ARG_ID_STRING WSLC_CLI_HELP_ARG
#define NO_ALIAS L""

using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc {
// An argument to a command.
struct Argument
{
    // Default argument configuration constants
    static constexpr Kind DefaultKind = Kind::Flag;
    static constexpr bool DefaultRequired = false;
    static constexpr int DefaultCountLimit = 1;

    // Full constructor with all parameters
    Argument(ArgType argType, std::wstring name, std::wstring alias, std::wstring desc, argument::Kind kind = DefaultKind, bool required = DefaultRequired, int countLimit = DefaultCountLimit) :
        m_argType(argType), m_name(name), m_alias(alias), m_desc(desc), m_type(kind), m_required(required), m_countLimit(countLimit)
    {
    }

    Argument(const Argument&) = default;
    Argument& operator=(const Argument&) = default;

    Argument(Argument&&) = default;
    Argument& operator=(Argument&&) = default;

    // Creates an argument with optional overrides for table defaults
    static Argument Create(
        ArgType type,
        std::optional<bool> required = std::nullopt,
        std::optional<int> countLimit = std::nullopt,
        std::optional<std::wstring> desc = std::nullopt);

    // Gets the argument usage string in the format of "-alias,--name" or just "--name" if no alias.
    std::wstring GetUsageString() const;

    // Arguments are not localized, but the description is.
    std::wstring Name() const
    {
        return m_name;
    }
    std::wstring Alias() const
    {
        return m_alias;
    }
    const std::wstring Description() const
    {
        return m_desc;
    }
    bool Required() const
    {
        return m_required;
    }
    ArgType Type() const
    {
        return m_argType;
    }
    Kind Kind() const
    {
        return m_type;
    }
    int Limit() const
    {
        return m_countLimit;
    }

private:
    ArgType m_argType;
    std::wstring m_name;
    std::wstring m_desc;
    std::wstring m_alias;
    bool m_required = DefaultRequired;
    argument::Kind m_type = DefaultKind;
    int m_countLimit = DefaultCountLimit;
};
} // namespace wsl::windows::wslc