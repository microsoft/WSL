/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionCommand.h

Abstract:

    Declaration of SessionCommand command tree.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Session Command
struct SessionCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"session";
    SessionCommand(const std::wstring& parent) : Command(CommandName, parent)
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
struct SessionListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    SessionListCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Shell Command
struct SessionShellCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"shell";
    SessionShellCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
