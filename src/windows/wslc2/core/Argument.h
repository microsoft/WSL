// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "ArgumentTypes.h"
#include "LocalizeMacros.h"

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#define WSLC_CLI_ARG_ID_CHAR        L'-'
#define WSLC_CLI_ARG_ID_STRING      L"-"
#define WSLC_CLI_ARG_SPLIT_CHAR     L'='
#define WSLC_HELP_CHAR              L'?'
#define WSLC_CLI_HELP_ARG_STRING    L"?"
#define WSLC_CLI_HELP_ARG           WSLC_CLI_ARG_ID_STRING WSLC_CLI_HELP_ARG_STRING
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc
{
    // An argument to a command.
    struct Argument
    {
        // Default no alias value
        constexpr static wchar_t NoAlias = L'\0';

        // Full constructor with all parameters
        Argument(
            ArgType argType,
            std::wstring name,
            wchar_t alias,
            std::wstring desc,
            argument::Kind kind = Kind::Standard,
            Visibility visibility = Visibility::Help,
            bool required = false,
            size_t countLimit = 1,
            Category category = Category::None,
            ExclusiveSet exclusiveSet = ExclusiveSet::None) :
            m_argType(argType), m_name(name), m_alias(alias), m_desc(desc), m_type(kind),
            m_visibility(visibility), m_required(required), m_countLimit(countLimit),
            m_category(category), m_exclusiveSet(exclusiveSet) {}

        ~Argument() = default;

        Argument(const Argument&) = default;
        Argument& operator=(const Argument&) = default;

        Argument(Argument&&) = default;
        Argument& operator=(Argument&&) = default;

        // Gets the argument for the given type.
        static Argument ForType(ArgType type);

        // Gets the common arguments for all commands.
        static void GetCommon(std::vector<Argument>& args);

        // Static argument validation helpers; throw CommandException when validation fails.

        // Requires that at most one argument from the list is present.
        static void ValidateExclusiveArguments(const Args& args);

        // Requires that if an argument depends on another one, it is not present without the dependency.
        static void ValidateArgumentDependency(const Args& args, ArgType type, ArgType dependencyArgType);

        static Category GetCategoriesPresent(const Args& args);

        // Requires that arguments meet common requirements
        static Category GetCategoriesAndValidateCommonArguments(const Args& args);
        static void ValidateCommonArguments(const Args& args) { std::ignore = GetCategoriesAndValidateCommonArguments(args); }

        // Gets the argument usage string in the format of "-alias,--name".
        std::wstring GetUsageString() const;

        // Arguments are not localized, but the description is.
        std::wstring Name() const { return m_name; }
        wchar_t Alias() const { return m_alias; }
        std::wstring AlternateName() const { return m_alternateName; }
        const std::wstring Description() const { return m_desc; }
        bool Required() const { return m_required; }
        ArgType Type() const { return m_argType; }
        Kind Kind() const { return m_type; }
        size_t Limit() const { return m_countLimit; }
        Visibility GetVisibility() const;

        Argument& SetRequired(bool required) { m_required = required; return *this; }
        Argument& SetCountLimit(size_t countLimit) { m_countLimit = countLimit; return *this; }

    private:
        ArgType m_argType;
        std::wstring m_name;
        mutable std::wstring m_desc;  // Mutable to allow lazy resolution
        wchar_t m_alias;
        std::wstring m_alternateName = std::wstring();
        bool m_required = false;
        argument::Kind m_type = Kind::Standard;
        Visibility m_visibility = Visibility::Help;
        Category m_category = Category::None;
        ExclusiveSet m_exclusiveSet = ExclusiveSet::None;
        size_t m_countLimit = 1;
    };
}