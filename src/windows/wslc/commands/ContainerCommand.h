/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerCommand.h

Abstract:

    Declaration of command classes and interfaces.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Container Command
struct ContainerCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"container";
    ContainerCommand(std::wstring parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct ContainerListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    ContainerListCommand(std::wstring parent) : Command(CommandName, {L"ls", L"ps"}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc