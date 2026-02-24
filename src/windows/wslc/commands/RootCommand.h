/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RootCommand.h

Abstract:

    Declaration of the RootCommand, which is the root of all commands in the CLI.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
struct RootCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"root";

    RootCommand() : Command(CommandName, {})
    {
    }

    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    virtual void ExecuteInternal(CLIExecutionContext& context) const;
};
} // namespace wsl::windows::wslc
