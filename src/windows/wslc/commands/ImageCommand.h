/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageCommand.h

Abstract:

    Declaration of command classes and interfaces.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Image Command
struct ImageCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"image";
    ImageCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Inspect Command
struct ImageInspectCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"inspect";
    ImageInspectCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct ImageListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    ImageListCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Load Command
struct ImageLoadCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"load";
    ImageLoadCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Prune Command
struct ImagePruneCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"prune";
    ImagePruneCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Pull Command
struct ImagePullCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"pull";
    ImagePullCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Push Command
struct ImagePushCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"push";
    ImagePushCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Save Command
struct ImageSaveCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"save";
    ImageSaveCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Tag Command
struct ImageTagCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"tag";
    ImageTagCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

} // namespace wsl::windows::wslc
