/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.cpp

Abstract:

    Implementation of the version command.

--*/
#include "VersionCommand.h"

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

void VersionCommand::PrintVersion()
{
    wsl::windows::common::wslutil::PrintMessage(std::format(L"{} {}", s_ExecutableName, WSL_PACKAGE_VERSION));
}

void VersionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    UNREFERENCED_PARAMETER(context);
    PrintVersion();
}
} // namespace wsl::windows::wslc
