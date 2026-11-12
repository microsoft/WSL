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
    ComposeCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::vector<Argument> GetGlobalArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    std::vector<std::unique_ptr<Command>> CreateCommands() const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Base for compose subcommands; every subcommand also accepts --dry-run.
struct ComposeSubcommand : public Command
{
    using Command::Command;

    std::vector<Argument> GetArguments() const final;

protected:
    virtual std::vector<Argument> GetComposeArguments() const = 0;
};

struct ComposeAttachCommand final : public ComposeSubcommand
{
    constexpr static std::wstring_view CommandName = L"attach";
    ComposeAttachCommand(const std::wstring& parent) : ComposeSubcommand(CommandName, parent)
    {
    }

    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    std::vector<Argument> GetComposeArguments() const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ComposeCreateCommand final : public ComposeSubcommand
{
    constexpr static std::wstring_view CommandName = L"create";
    ComposeCreateCommand(const std::wstring& parent) : ComposeSubcommand(CommandName, parent)
    {
    }

    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    std::vector<Argument> GetComposeArguments() const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ComposeStartCommand final : public ComposeSubcommand
{
    constexpr static std::wstring_view CommandName = L"start";
    ComposeStartCommand(const std::wstring& parent) : ComposeSubcommand(CommandName, parent)
    {
    }

    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    std::vector<Argument> GetComposeArguments() const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ComposeStopCommand final : public ComposeSubcommand
{
    constexpr static std::wstring_view CommandName = L"stop";
    ComposeStopCommand(const std::wstring& parent) : ComposeSubcommand(CommandName, parent)
    {
    }

    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    std::vector<Argument> GetComposeArguments() const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

struct ComposeUpCommand final : public ComposeSubcommand
{
    constexpr static std::wstring_view CommandName = L"up";
    ComposeUpCommand(const std::wstring& parent) : ComposeSubcommand(CommandName, parent)
    {
    }

    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    std::vector<Argument> GetComposeArguments() const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

} // namespace wsl::windows::wslc
