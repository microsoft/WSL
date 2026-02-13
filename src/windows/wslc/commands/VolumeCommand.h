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
    VolumeCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Create Command
struct VolumeCreateCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"create";
    VolumeCreateCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Delete Command
struct VolumeDeleteCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"delete";
    VolumeDeleteCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Inspect Command
struct VolumeInspectCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"inspect";
    VolumeInspectCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct VolumeListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    VolumeListCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Prune Command
struct VolumePruneCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"prune";
    VolumePruneCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Save Command
struct VolumeSaveCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"save";
    VolumeSaveCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
