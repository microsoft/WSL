/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.cpp

Abstract:

    Implementation of the version command.

--*/

#include "VersionCommand.h"
#include "ArgumentConvertedTypes.h"
#include "CLIExecutionContext.h"
#include "JsonUtils.h"

using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;

namespace wsl::windows::wslc {
std::vector<Argument> VersionCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Format),
    };
}

std::wstring VersionCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VersionDesc();
}

std::wstring VersionCommand::LongDescription() const
{
    return Localization::WSLCCLI_VersionLongDesc();
}

void VersionCommand::PrintVersion(Terminal& terminal)
{
    terminal.Output(L"{} {}\n", s_ExecutableName, WSL_PACKAGE_VERSION);
}

void VersionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    const auto format = context.Args.GetValue<ArgType::Format>(FormatType::Table);

    switch (format)
    {
    case FormatType::Json:
    {
        nlohmann::json root;
        root["Client"]["Version"] = std::string{WSL_PACKAGE_VERSION};
        context.Terminal.Output(L"{}\n", MultiByteToWide(root.dump(c_jsonCompactIndent)));
        break;
    }
    case FormatType::Table:
        PrintVersion(context.Terminal);
        break;
    default:
        THROW_HR(E_UNEXPECTED);
    }
}
} // namespace wsl::windows::wslc
