/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.cpp

Abstract:

    Implementation of the version command.

--*/
#include "VersionCommand.h"

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
std::wstring VersionCommand::ShortDescription() const
{
    return {L"Show version information."};
}

std::wstring VersionCommand::LongDescription() const
{
    return {L"Show version information for this tool."};
}

void VersionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    wsl::windows::common::wslutil::PrintMessage(std::format(L"{} v{}", s_ExecutableName, WSL_PACKAGE_VERSION));
}
} // namespace wsl::windows::wslc
