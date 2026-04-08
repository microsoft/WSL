/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeCommand.h

Abstract:

    Declaration of command classes and interfaces.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Volume Command
struct VolumeCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"volume";
    VolumeCommand(const std::wstring& parent) : Command(CommandName, parent)
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
struct VolumeCreateCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"create";
    VolumeCreateCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Delete Command
struct VolumeDeleteCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"remove";
    VolumeDeleteCommand(const std::wstring& parent) : Command(CommandName, {L"delete", L"rm"}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Inspect Command
struct VolumeInspectCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"inspect";
    VolumeInspectCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct VolumeListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    VolumeListCommand(const std::wstring& parent) : Command(CommandName, {L"ls"}, parent)
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
