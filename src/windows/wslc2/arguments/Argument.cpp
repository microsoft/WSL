// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
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

namespace wsl::windows::wslc
{
    using namespace wsl::windows::wslc::execution;

    namespace
    {
        bool ContainsArgumentFromList(const Args& args, const std::vector<ArgType>& argTypes)
        {
            return std::any_of(argTypes.begin(), argTypes.end(), [&](ArgType arg) { return args.Contains(arg); });
        }

        // Helper to get enum name as string for comparison
        std::wstring GetArgTypeName(ArgType type)
        {
            switch (type)
            {
#define WSLC_ARG_NAME_CASE(EnumName, Name, Alias, Desc, DataType, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet) \
            case ArgType::EnumName: \
                return L#EnumName;

            WSLC_ARGUMENTS(WSLC_ARG_NAME_CASE)
#undef WSLC_ARG_NAME_CASE

            default:
                return L"Unknown";
            }
        }
    }

    Argument Argument::ForType(ArgType type)
    {
        switch (type)
        {
#define WSLC_ARG_CASE(EnumName, Name, Alias, Desc, DataType, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet) \
        case ArgType::EnumName: \
            return Argument{type, L##Name, Alias, Desc, Kind, Visibility, Required, CountLimit, Category, ExclusiveSet};

            WSLC_ARGUMENTS(WSLC_ARG_CASE)
#undef WSLC_ARG_CASE

        default:
            THROW_HR(E_UNEXPECTED);
        }
    }

    void Argument::GetCommon(std::vector<Argument>& args)
    {
        args.push_back(ForType(ArgType::Help));
    }

    std::wstring Argument::GetUsageString() const
    {
        std::wostringstream strstr;
        if (m_alias != NoAlias)
        {
            strstr << WSLC_CLI_ARG_ID_CHAR << m_alias << L',';
        }
        if (!m_alternateName.empty())
        {
            strstr << WSLC_CLI_ARG_ID_CHAR << WSLC_CLI_ARG_ID_CHAR << m_alternateName << L',';
        }
        strstr << WSLC_CLI_ARG_ID_CHAR << WSLC_CLI_ARG_ID_CHAR << m_name;
        return strstr.str();
    }

    void Argument::ValidateExclusiveArguments(const Args& args)
    {
        auto keys = args.GetKeys();
        
        using ExclusiveSet_t = std::underlying_type_t<ExclusiveSet>;
        for (ExclusiveSet_t i = 1 + static_cast<ExclusiveSet_t>(ExclusiveSet::None); i < static_cast<ExclusiveSet_t>(ExclusiveSet::Max); i <<= 1)
        {
            std::vector<ArgType> argsFromSet;
            std::copy_if(
                keys.begin(),
                keys.end(),
                std::back_inserter(argsFromSet),
                [=](ArgType arg) { 
                    auto argument = Argument::ForType(arg);
                    return static_cast<ExclusiveSet_t>(argument.m_exclusiveSet) & i; 
                });

            if (argsFromSet.size() > 1)
            {
                // Create a string showing the exclusive args.
                std::wstring argsString;
                for (const auto& arg : argsFromSet)
                {
                    if (!argsString.empty())
                    {
                        argsString += L'|';
                    }
                    argsString += Argument::ForType(arg).Name();
                }

                throw CommandException(Localization::WSLCCLI_MultipleExclusiveArgumentsProvided(argsString));
            }
        }
    }

    void Argument::ValidateArgumentDependency(const Args& args, ArgType type, ArgType dependencyArgType)
    {
        if (args.Contains(type) && !args.Contains(dependencyArgType))
        {
            throw CommandException(Localization::WSLCCLI_DependencyArgumentMissing(
                Argument::ForType(type).Name(),
                Argument::ForType(dependencyArgType).Name()));
        }
    }

    Category Argument::GetCategoriesPresent(const Args& args)
    {
        auto keys = args.GetKeys();

        Category result = Category::None;
        for (const auto& argType : keys)
        {
            result |= Argument::ForType(argType).m_category;
        }

        return result;
    }

    Category Argument::GetCategoriesAndValidateCommonArguments(const Args& args)
    {
        const auto categories = GetCategoriesPresent(args);

        // Do common argument validation here.
        return categories;
    }

    Visibility Argument::GetVisibility() const
    {
        // Visibility adjustments, such as experimental or disabled by policy.
        return m_visibility;
    }
}
