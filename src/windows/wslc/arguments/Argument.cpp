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

// This is the main Argument creation method, allowing overrides of the default properties of arguments.
// The ArgType has some core characteristic, such as the Kind, Name, and Alias. If these
// need to be changed, it is recommended to create a new ArgType in ArgumentDefinitions.h. If the argument
// just needs a different description, it can be overridden in the desc, or if you need it to be required,
// or to allow multiple uses within a command, then those properties can be set using the Create
// function below inside the command. In this way all arguments default to "1" use and not required, and
// this can only be changed in the command's GetArguments function, so the defaults are always clear and
// consistent. Visibility can also be overridden and is defaulted to "Help".
Argument Argument::Create(ArgType type, std::optional<bool> required, std::optional<int> countLimit, std::optional<std::wstring> desc)
{
    switch (type)
    {
#define WSLC_ARG_CREATE_CASE(EnumName, Name, Alias, ArgumentKind, Desc) \
    case ArgType::EnumName: \
        return Argument{ \
            type, \
            L##Name, \
            Alias, \
            desc.has_value() ? std::move(desc.value()) : std::wstring(Desc), \
            ArgumentKind, \
            required.value_or(DefaultRequired), \
            countLimit.value_or(DefaultCountLimit)};

        WSLC_ARGUMENTS(WSLC_ARG_CREATE_CASE)
#undef WSLC_ARG_CREATE_CASE

    default:
        THROW_HR(E_UNEXPECTED);
    }
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
