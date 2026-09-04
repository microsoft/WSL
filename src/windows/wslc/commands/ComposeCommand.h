/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeCommand.h

Abstract:

    Declares the minimal compose command tree.

--*/

#pragma once

#include "Command.h"

namespace wsl::windows::wslc {

struct ComposeCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"compose";
    ComposeCommand(const std::wstring& Parent) : Command(CommandName, Parent)
    {
    }

    std::vector<std::unique_ptr<Command>> GetCommands() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

struct ComposeCreateCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"create";
    ComposeCreateCommand(const std::wstring& Parent) : Command(CommandName, Parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

struct ComposeListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    ComposeListCommand(const std::wstring& Parent) : Command(CommandName, {L"ls"}, Parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

struct ComposeUpCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"up";
    ComposeUpCommand(const std::wstring& Parent) : Command(CommandName, Parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

struct ComposeStartCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"start";
    ComposeStartCommand(const std::wstring& Parent) : Command(CommandName, Parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

struct ComposeAttachCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"attach";
    ComposeAttachCommand(const std::wstring& Parent) : Command(CommandName, Parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

struct ComposeStopCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"stop";
    ComposeStopCommand(const std::wstring& Parent) : Command(CommandName, Parent)
    {
    }

    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& Context) const override;
};

} // namespace wsl::windows::wslc
