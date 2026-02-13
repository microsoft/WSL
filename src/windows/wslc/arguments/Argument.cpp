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

        // Helper to get enum name as string for comparison purposes. This is not for display to users.
        std::wstring GetArgTypeName(ArgType type)
        {
            switch (type)
            {
#define WSLC_ARG_NAME_CASE(EnumName, Name, Alias, ArgumentKind, Desc) \
            case ArgType::EnumName: \
                return L#EnumName;

            WSLC_ARGUMENTS(WSLC_ARG_NAME_CASE)
#undef WSLC_ARG_NAME_CASE

            default:
                return L"Unknown";
            }
        }
    }

    // This is the main Argument creation method, allowing overrides of the default properties of arguments.
    // The ArgType has some core characteristic, such as the Kind, Name, and Alias. If these
    // need to be changed, it is recommended to create a new ArgType in ArgumentDefinitions.h. If the argument
    // just needs a different description, it can be overridden in the desc, or if you need it to be required,
    // or to allow multiple uses within a command, then those properties can be set using the Create
    // function below inside the command. In this way all arguments default to "1" use and not required, and
    // this can only be changed in the command's GetArguments function, so the defaults are always clear and
    // consistent. Visibility can also be overridden and is defaulted to "Help".
    Argument Argument::Create(
        ArgType type,
        std::optional<bool> required,
        std::optional<int> countLimit,
        std::optional<std::wstring> desc,
        std::optional<Visibility> visibility)
    {
        switch (type)
        {
#define WSLC_ARG_CREATE_CASE(EnumName, Name, Alias, ArgumentKind, Desc) \
    case ArgType::EnumName: \
        return Argument{type, \
                       L##Name, \
                       Alias, \
                       desc.value_or(Desc), \
                       ArgumentKind, \
                       visibility.value_or(Visibility::Help), \
                       required.value_or(false), \
                       countLimit.value_or(1)};

        WSLC_ARGUMENTS(WSLC_ARG_CREATE_CASE)
#undef WSLC_ARG_CREATE_CASE

        default:
            THROW_HR(E_UNEXPECTED);
        }
    }

    // Arguments common to ALL commands are defined here so they do not need to be added each time.
    // This starts with Help, but if there are other arguments that are common, they can be added.
    void Argument::GetCommon(std::vector<Argument>& args)
    {
        args.push_back(Create(ArgType::Help));
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
}
