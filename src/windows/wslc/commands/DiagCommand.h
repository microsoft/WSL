/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagCommand.h

Abstract:

    Declaration of DiagCommand command tree.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
// Root Diag Command
struct DiagCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"diag";
    DiagCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

    std::vector<std::unique_ptr<Command>> GetCommands() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Attach Command
struct DiagAttachCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"attach";
    DiagAttachCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Build Command
struct DiagBuildCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"build";
    DiagBuildCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct DiagListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    DiagListCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Logs Command
struct DiagLogsCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"logs";
    DiagLogsCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Pull Command
struct DiagPullCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"pull";
    DiagPullCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Run Command
struct DiagRunCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"run";
    DiagRunCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Shell Command
struct DiagShellCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"shell";
    DiagShellCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring_view ShortDescription() const override;
    std::wstring_view LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

} // namespace wsl::windows::wslc
