/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.cpp

Abstract:

    Implementation of the version command.

--*/
#include "VersionCommand.h"
#include "CLIExecutionContext.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
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
    PrintVersion(context.Reporter);
}
} // namespace wsl::windows::wslc
