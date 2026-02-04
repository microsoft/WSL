// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "argument.h"
#include "command.h"
#include "context.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    using namespace wsl::windows::wslc::execution;

    namespace
    {
        bool ContainsArgumentFromList(const Args& args, const std::vector<Args::Type>& argTypes)
        {
            return std::any_of(argTypes.begin(), argTypes.end(), [&](Args::Type arg) { return args.Contains(arg); });
        }
    }

    ArgumentCommon ArgumentCommon::ForType(Args::Type type)
    {
        switch (type)
        {
        // Common arguments
        case Args::Type::Help:
            return { type, L"help", WSLC_CLI_HELP_ARGUMENT_TEXT_CHAR };
        case Args::Type::SessionId:
            return {type, L"session", L's'};
        case Args::Type::Info:
            return {type, L"info", L'i' };

        // Used for demonstration purposes
        case Args::Type::TestArg:
            return {type, L"arg", L'a' };

        // Container
        case Args::Type::Attach:
            return {type, L"attach", L'a'};
        case Args::Type::Interactive:
            return {type, L"interactive", L'i'};
        case Args::Type::ContainerId:
            return {type, L"containerid", ArgTypeCategory::ContainerSelection};

        default:
            THROW_HR(E_UNEXPECTED);
        }
    }

    std::vector<ArgumentCommon> ArgumentCommon::GetFromExecArgs(const Args& execArgs)
    {
        auto argTypes = execArgs.GetTypes();
        std::vector<ArgumentCommon> result;
        std::transform(argTypes.begin(), argTypes.end(), std::back_inserter(result), ArgumentCommon::ForType);
        return result;
    }

    Argument Argument::ForType(Args::Type type)
    {
        switch (type)
        {
        case Args::Type::Help:
            return Argument{ type, Localization::WSLCCLI_HelpArgumentDescription(), ArgumentType::Flag };
        case Args::Type::Info:
            return Argument{ type, Localization::WSLCCLI_InfoArgumentDescription(), ArgumentType::Flag };
        case Args::Type::SessionId:
            return Argument{type, L"Session Id", ArgumentType::Standard};
        case Args::Type::Attach:
            return Argument{type, L"Attach to stdout/stderr", ArgumentType::Flag };
        case Args::Type::Interactive:
            return Argument{type, L"Interactive terminal", ArgumentType::Flag };
        case Args::Type::ContainerId:
            return Argument{type, L"Container Id", ArgumentType::Positional, Visibility::Example, true};
        case Args::Type::TestArg:
            return Argument{type, L"Display ninjacat", ArgumentType::Flag, true };
        default:
            THROW_HR(E_UNEXPECTED);
        }
    }

    void Argument::GetCommon(std::vector<Argument>& args)
    {
        args.push_back(ForType(Args::Type::Help));
        args.push_back(ForType(Args::Type::SessionId));
    }

    std::wstring Argument::GetUsageString() const
    {
        std::wostringstream strstr;
        if (Alias() != ArgumentCommon::NoAlias)
        {
            strstr << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << Alias() << ',';
        }
        if (AlternateName() != Argument::NoAlternateName)
        {
            strstr << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << AlternateName() << ',';
        }
        strstr << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << WSLC_CLI_ARGUMENT_IDENTIFIER_CHAR << Name();
        return strstr.str();
    }

    void Argument::ValidateExclusiveArguments(const Args& args)
    {
        auto argProperties = ArgumentCommon::GetFromExecArgs(args);

        using ExclusiveSet_t = std::underlying_type_t<ArgTypeExclusiveSet>;
        for (ExclusiveSet_t i = 1 + static_cast<ExclusiveSet_t>(ArgTypeExclusiveSet::None); i < static_cast<ExclusiveSet_t>(ArgTypeExclusiveSet::Max); i <<= 1)
        {
            std::vector<ArgumentCommon> argsFromSet;
            std::copy_if(
                argProperties.begin(),
                argProperties.end(),
                std::back_inserter(argsFromSet),
                [=](const ArgumentCommon& arg) { return static_cast<ExclusiveSet_t>(arg.ExclusiveSet) & i; });

            if (argsFromSet.size() > 1)
            {
                // Create a string showing the exclusive args.
                std::wstring argsString;
                for (const auto& arg : argsFromSet)
                {
                    if (!argsString.empty())
                    {
                        argsString += '|';

                    }

                    argsString += arg.Name;
                }

                throw CommandException(Localization::WSLCCLI_MultipleExclusiveArgumentsProvided(std::wstring{ argsString }));
            }
        }
    }

    void Argument::ValidateArgumentDependency(const Args& args, Args::Type type, Args::Type dependencyArgType)
    {
        if (args.Contains(type) && !args.Contains(dependencyArgType))
        {
            throw CommandException(Localization::WSLCCLI_DependencyArgumentMissing(
                std::wstring{ ArgumentCommon::ForType(type).Name },
                std::wstring{ ArgumentCommon::ForType(dependencyArgType).Name }));
        }
    }

    ArgTypeCategory Argument::GetCategoriesPresent(const Args& args)
    {
        auto argProperties = ArgumentCommon::GetFromExecArgs(args);

        ArgTypeCategory result = ArgTypeCategory::None;
        for (const auto& arg : argProperties)
        {
            result |= arg.TypeCategory;
        }

        return result;
    }

    ArgTypeCategory Argument::GetCategoriesAndValidateCommonArguments(const Args& args)
    {
        const auto categories = GetCategoriesPresent(args);

        // Do common argument validation here.
        return categories;
    }

    Argument::Visibility Argument::GetVisibility() const
    {
        // Visibility adjustments, such as experimental or disabled by policy.
        return m_visibility;
    }
}
