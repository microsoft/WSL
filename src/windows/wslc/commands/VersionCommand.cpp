/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.cpp

Abstract:

    Implementation of the version command.

--*/

#include "VersionCommand.h"
#include "ArgumentValidation.h"
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

void VersionCommand::PrintVersion(Reporter& reporter)
{
    reporter.Output(L"{} {}\n", s_ExecutableName, WSL_PACKAGE_VERSION);
}

void VersionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    FormatType format = FormatType::Table;
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        nlohmann::json root;
        root["Client"]["Version"] = std::string{WSL_PACKAGE_VERSION};
        context.Reporter.Output(L"{}\n", MultiByteToWide(root.dump(c_jsonPrettyPrintIndent)));
        break;
    }
    case FormatType::Table:
        PrintVersion(context.Reporter);
        break;
    default:
        THROW_HR(E_UNEXPECTED);
    }
}
} // namespace wsl::windows::wslc
