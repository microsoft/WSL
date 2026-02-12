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
        bool ContainsArgumentFromList(const ArgMap& args, const std::vector<ArgType>& argTypes)
        {
            return std::any_of(argTypes.begin(), argTypes.end(), [&](ArgType arg) { return args.Contains(arg); });
        }

        // Helper to get enum name as string for comparison
        std::wstring GetArgTypeName(ArgType type)
        {
            switch (type)
            {
#define WSLC_ARG_NAME_CASE(EnumName, Name, Alias, Kind, DataType, Desc) \
            case ArgType::EnumName: \
                return L#EnumName;

            WSLC_ARGUMENTS(WSLC_ARG_NAME_CASE)
#undef WSLC_ARG_NAME_CASE

            default:
                return L"Unknown";
            }
        }
    }

    Argument Argument::Create(
        ArgType type,
        std::optional<bool> required,
        std::optional<int> countLimit,
        std::optional<std::wstring> desc,
        std::optional<Visibility> visibility)
    {
        switch (type)
        {
#define WSLC_ARG_CREATE_CASE(EnumName, Name, Alias, Kind, DataType, Desc) \
        case ArgType::EnumName: \
            return Argument{type, \
                           L##Name, \
                           Alias, \
                           desc.value_or(Desc), \
                           Kind, \
                           visibility.value_or(Visibility::Help), \
                           required.value_or(false), \
                           countLimit.value_or(1)};

            WSLC_ARGUMENTS(WSLC_ARG_CREATE_CASE)
#undef WSLC_ARG_CREATE_CASE

        default:
            THROW_HR(E_UNEXPECTED);
        }
    }


    void Argument::GetCommon(std::vector<Argument>& args)
    {
        args.push_back(Create(ArgType::Help));
    }

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
}
