/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Argument.h

Abstract:

    Declaration of the Argument class for command-line argument handling.

--*/
#pragma once
#include "ArgMap.h"

#include <optional>
#include <string>
#include <utility>

#define WSLC_CLI_ARG_ID_CHAR L'-'
#define WSLC_CLI_ARG_ID_STRING L"-"
#define WSLC_CLI_ARG_SPLIT_CHAR L'='
#define WSLC_CLI_HELP_ARG L"?"
#define WSLC_CLI_HELP_ARG_STRING WSLC_CLI_ARG_ID_STRING WSLC_CLI_HELP_ARG
#define NO_ALIAS L""

using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc {
struct ArgumentOverrides
{
    std::optional<std::wstring> Name;
    std::optional<std::wstring> Alias;
    std::optional<bool> Required;
    std::optional<argument::Limit> Limit;
    std::optional<std::wstring> Desc;
};

// An argument to a command.
struct Argument
{
    // Default argument configuration constants
    static constexpr Kind DefaultKind = Kind::Flag;
    static constexpr bool DefaultRequired = false;
    static constexpr argument::Limit DefaultLimit = argument::Limit::Single;

    // Full constructor with all parameters
    Argument(
        ArgType argType,
        std::wstring name,
        std::wstring alias,
        std::wstring desc,
        argument::Kind kind = DefaultKind,
        bool required = DefaultRequired,
        argument::Limit limit = DefaultLimit) :
        m_argType(argType), m_name(std::move(name)), m_desc(std::move(desc)), m_alias(std::move(alias)), m_required(required), m_type(kind), m_limit(limit)
    {
    }

    Argument(const Argument&) = default;
    Argument& operator=(const Argument&) = default;

    Argument(Argument&&) = default;
    Argument& operator=(Argument&&) = default;

    // Creates an argument using its table defaults and any command-specific overrides.
    static Argument Create(ArgType type, ArgumentOverrides overrides = {});

    // Gets the argument usage string in the format of "-alias,--name" or just "--name" if no alias.
    std::wstring GetUsageString() const;

    // Arguments are not localized, but the description is.
    const std::wstring& Name() const
    {
        return m_name;
    }
    const std::wstring& Alias() const
    {
        return m_alias;
    }
    const std::wstring& Description() const
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
    bool IsOption() const
    {
        return m_type == argument::Kind::Flag || m_type == argument::Kind::Value;
    }
    Limit Limit() const
    {
        return m_limit;
    }

    // A single-value argument accepts one value (last-wins on repeats).
    bool IsSingle() const
    {
        return m_limit == argument::Limit::Single;
    }

    // An unlimited argument accumulates every value supplied.
    bool IsUnlimited() const
    {
        return m_limit == argument::Limit::Unlimited;
    }

    // Validates this argument's current values, caching the converted result (converted arguments
    // only) on `execArgs` so reads reuse it without re-parsing until the raw values change.
    void Validate(ArgMap& execArgs) const;

private:
    ArgType m_argType;
    std::wstring m_name;
    std::wstring m_desc;
    std::wstring m_alias;
    bool m_required = DefaultRequired;
    argument::Kind m_type = DefaultKind;
    argument::Limit m_limit = DefaultLimit;
};
} // namespace wsl::windows::wslc
