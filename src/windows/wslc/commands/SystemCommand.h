/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SystemCommand.h

Abstract:

    Declaration of System command tree.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root System Command
struct SystemCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"system";
    SystemCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
