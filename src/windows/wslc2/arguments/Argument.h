// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ArgumentTypes.h"

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#define WSLC_CLI_ARG_ID_CHAR        L'-'
#define WSLC_CLI_ARG_ID_STRING      L"-"
#define WSLC_CLI_ARG_SPLIT_CHAR     L'='
#define WSLC_CLI_HELP_ARG           L"?"
#define WSLC_CLI_HELP_ARG_STRING    WSLC_CLI_ARG_ID_STRING WSLC_CLI_HELP_ARG
#define NO_ALIAS                    L""

using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc
{
    // An argument to a command.
    struct Argument
    {
        // Full constructor with all parameters
        Argument(
            ArgType argType,
            std::wstring name,
            std::wstring alias,
            std::wstring desc,
            argument::Kind kind = Kind::Standard,
            Visibility visibility = Visibility::Help,
            bool required = false,
            size_t countLimit = 1) :
            m_argType(argType), m_name(name), m_alias(alias), m_desc(desc), m_type(kind),
            m_visibility(visibility), m_required(required), m_countLimit(countLimit) {}

        ~Argument() = default;

        Argument(const Argument&) = default;
        Argument& operator=(const Argument&) = default;

        Argument(Argument&&) = default;
        Argument& operator=(Argument&&) = default;

        // Creates an argument with optional overrides for table defaults
        static Argument Create(
            ArgType type,
            std::optional<std::wstring> desc = std::nullopt,
            std::optional<bool> required = std::nullopt,
            std::optional<size_t> countLimit = std::nullopt,
            std::optional<Visibility> visibility = std::nullopt);

        // Gets the common arguments for all commands.
        static void GetCommon(std::vector<Argument>& args);

        // Gets the argument usage string in the format of "-alias,--name" or just "--name" if no alias.
        std::wstring GetUsageString() const;

        // Arguments are not localized, but the description is.
        std::wstring Name() const { return m_name; }
        std::wstring Alias() const { return m_alias; }
        const std::wstring Description() const { return m_desc; }
        bool Required() const { return m_required; }
        ArgType Type() const { return m_argType; }
        Kind Kind() const { return m_type; }
        size_t Limit() const { return m_countLimit; }
        Visibility GetVisibility() const { return m_visibility; }

        Argument& SetRequired(bool required) { m_required = required; return *this; }
        Argument& SetCountLimit(size_t countLimit) { m_countLimit = countLimit; return *this; }

        // Validates this argument's value in the provided args
        void Validate(const Args& execArgs) const;

    private:
        ArgType m_argType;
        std::wstring m_name;
        std::wstring m_desc;
        std::wstring m_alias;
        bool m_required = false;
        argument::Kind m_type = Kind::Standard;
        Visibility m_visibility = Visibility::Help;
        size_t m_countLimit = 1;
    };
}