/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.cpp

Abstract:

    Implementation of the version command.

--*/
#include "VersionCommand.h"
#include "VersionTasks.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

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

void VersionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << GetVersionInfo //
            << ListVersionInfo;
}
} // namespace wsl::windows::wslc
