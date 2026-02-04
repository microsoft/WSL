// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "ExecutionContext.h"

#include <string>
#include <string_view>
#include <vector>

#define WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR       L'-'
#define WSLC_CLI_ARGUMENT_IDENTIFIER_STRING     L"-"
#define WSLC_CLI_ARGUMENT_SPLIT_CHAR            L'='
#define WSLC_CLI_HELP_ARGUMENT_TEXT_CHAR        L'?'
#define WSLC_CLI_HELP_ARGUMENT_TEXT_STRING      L"?"
#define WSLC_CLI_HELP_ARGUMENT                  WSLC_CLI_ARGUMENT_IDENTIFIER_STRING WSLC_CLI_HELP_ARGUMENT_TEXT_STRING

namespace wsl::windows::wslc
{
    // The type of argument.
    enum class ArgumentType
    {
        // Argument requires specifying the name before the value.
        Standard,
        // Argument value can be specified alone; position indicates argument name.
        Positional,
        // Only argument name can be specified and indicates a bool value.
        Flag,
    };

    // Categories an arg type can belong to.
    // Used to reason about the arguments present without having to repeat the same
    // lists every time.
    enum class ArgTypeCategory : uint32_t
    {
        None = 0x0,
        ContainerSelection = 0x1,      // Args that select a container.
        ImageSelection = 0x2,          // Args that select an image.
    };

    DEFINE_ENUM_FLAG_OPERATORS(ArgTypeCategory);

    // Exclusive sets an argument can belong to.
    // Only one argument from each exclusive set is allowed at a time.
    enum class ArgTypeExclusiveSet : uint32_t
    {
        None = 0x0,
        ProgressBarOption = 0x1,    // None or ANSI, may have other types
        TargetSelection = 0x2,      // For selecting things to stop/kill/delete (all vs specific)
        FormatOption = 0x4,         // Output Format options (JSON, table, etc)

        // This must always be at the end
        Max
    };

    DEFINE_ENUM_FLAG_OPERATORS(ArgTypeExclusiveSet);

    // An argument to a command; containing only data that is common to all its uses.
    // Argument extends this by adding command-specific values, like help strings.
    struct ArgumentCommon
    {
        // Defines an argument with no alias.
        constexpr static wchar_t NoAlias = L'\0';

        ArgumentCommon(Args::Type execArgType, std::wstring_view name, wchar_t alias, std::wstring_view alternateName, ArgTypeCategory typeCategory = ArgTypeCategory::None, ArgTypeExclusiveSet exclusiveSet = ArgTypeExclusiveSet::None)
            : Type(execArgType), Name(name), Alias(alias), AlternateName(alternateName), TypeCategory(typeCategory), ExclusiveSet(exclusiveSet) {}

        ArgumentCommon(Args::Type execArgType, std::wstring_view name, wchar_t alias, ArgTypeCategory typeCategory = ArgTypeCategory::None, ArgTypeExclusiveSet exclusiveSet = ArgTypeExclusiveSet::None)
            : Type(execArgType), Name(name), Alias(alias), TypeCategory(typeCategory), ExclusiveSet(exclusiveSet) {}

        ArgumentCommon(Args::Type execArgType, std::wstring_view name, std::wstring_view alternateName, ArgTypeCategory typeCategory = ArgTypeCategory::None, ArgTypeExclusiveSet exclusiveSet = ArgTypeExclusiveSet::None)
            : Type(execArgType), Name(name), Alias(NoAlias), AlternateName(alternateName), TypeCategory(typeCategory), ExclusiveSet(exclusiveSet) {}

        ArgumentCommon(Args::Type execArgType, std::wstring_view name, ArgTypeCategory typeCategory = ArgTypeCategory::None, ArgTypeExclusiveSet exclusiveSet = ArgTypeExclusiveSet::None)
            : Type(execArgType), Name(name), Alias(NoAlias), TypeCategory(typeCategory), ExclusiveSet(exclusiveSet) {}

        // Gets the argument for the given type.
        static ArgumentCommon ForType(Args::Type execArgType);

        static std::vector<ArgumentCommon> GetFromExecArgs(const Args& execArgs);

        Args::Type Type;
        std::wstring_view Name;
        wchar_t Alias;
        std::wstring_view AlternateName;
        ArgTypeCategory TypeCategory;
        ArgTypeExclusiveSet ExclusiveSet;
    };

    // An argument to a command.
    struct Argument
    {
        // Controls the visibility of the field.
        enum class Visibility
        {
            // Shown in the example.
            Example,
            // Shown only in the table below the example.
            Help,
            // Not shown in help.
            Hidden,
        };

        // Defines an argument with no alternate name
        constexpr static std::wstring_view NoAlternateName = L"";

        Argument(Args::Type execArgType, std::wstring desc) :
            m_argCommon(ArgumentCommon::ForType(execArgType)), m_desc(desc) {}

        Argument(Args::Type execArgType, std::wstring desc, bool required) :
            m_argCommon(ArgumentCommon::ForType(execArgType)), m_desc(desc), m_required(required) {}

        Argument(Args::Type execArgType, std::wstring desc, ArgumentType type) :
            m_argCommon(ArgumentCommon::ForType(execArgType)), m_desc(desc), m_type(type) {}

        Argument(Args::Type execArgType, std::wstring desc, ArgumentType type, Argument::Visibility visibility) :
            m_argCommon(ArgumentCommon::ForType(execArgType)), m_desc(desc), m_type(type), m_visibility(visibility) {}

        Argument(Args::Type execArgType, std::wstring desc, ArgumentType type, bool required) :
            m_argCommon(ArgumentCommon::ForType(execArgType)), m_desc(desc), m_type(type), m_required(required) {}

        Argument(Args::Type execArgType, std::wstring desc, ArgumentType type, Argument::Visibility visibility, bool required) :
            m_argCommon(ArgumentCommon::ForType(execArgType)), m_desc(desc), m_type(type), m_visibility(visibility), m_required(required) {}

        ~Argument() = default;

        Argument(const Argument&) = default;
        Argument& operator=(const Argument&) = default;

        Argument(Argument&&) = default;
        Argument& operator=(Argument&&) = default;

        // Gets the argument for the given type.
        static Argument ForType(Args::Type type);

        // Gets the common arguments for all commands.
        static void GetCommon(std::vector<Argument>& args);

        // Static argument validation helpers; throw CommandException when validation fails.

        // Requires that at most one argument from the list is present.
        static void ValidateExclusiveArguments(const Args& args);

        // Requires that if an argument depends on another one, it is not present without the dependency.
        static void ValidateArgumentDependency(const Args& args, Args::Type type, Args::Type dependencyArgType);

        static ArgTypeCategory GetCategoriesPresent(const Args& args);

        // Requires that arguments meet common requirements
        static ArgTypeCategory GetCategoriesAndValidateCommonArguments(const Args& args);
        static void ValidateCommonArguments(const Args& args) { std::ignore = GetCategoriesAndValidateCommonArguments(args); }

        // Gets the argument usage string in the format of "-alias,--name".
        std::wstring GetUsageString() const;

        // Arguments are not localized at this time.
        std::wstring_view Name() const { return m_argCommon.Name; }
        wchar_t Alias() const { return m_argCommon.Alias; }
        std::wstring_view AlternateName() const { return m_argCommon.AlternateName; }
        Args::Type ExecArgType() const { return m_argCommon.Type; }
        const std::wstring Description() const { return m_desc; }
        bool Required() const { return m_required; }
        ArgumentType Type() const { return m_type; }
        size_t Limit() const { return m_countLimit; }
        Argument::Visibility GetVisibility() const;

        Argument& SetRequired(bool required) { m_required = required; return *this; }
        Argument& SetCountLimit(size_t countLimit) { m_countLimit = countLimit; return *this; }

    private:
        ArgumentCommon m_argCommon;
        std::wstring m_desc;
        bool m_required = false;
        ArgumentType m_type = ArgumentType::Standard;
        Argument::Visibility m_visibility = Argument::Visibility::Example;
        size_t m_countLimit = 1;
    };
}
