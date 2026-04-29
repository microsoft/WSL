/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.h

Abstract:

    Declaration of the VersionCommand.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
struct VersionCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"version";
    VersionCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
