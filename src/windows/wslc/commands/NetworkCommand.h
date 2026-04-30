/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkCommand.h

Abstract:

    Declaration of command classes and interfaces.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Network Command
struct NetworkCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"network";
    NetworkCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Create Command
struct NetworkCreateCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"create";
    NetworkCreateCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Remove Command
struct NetworkRemoveCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"remove";
    NetworkRemoveCommand(const std::wstring& parent) : Command(CommandName, {L"delete", L"rm"}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Inspect Command
struct NetworkInspectCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"inspect";
    NetworkInspectCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct NetworkListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    NetworkListCommand(const std::wstring& parent) : Command(CommandName, {L"ls"}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(const ArgMap& execArgs) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
