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
    ContainerCommand(const std::wstring& parent) : Command(CommandName, parent)
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
struct ContainerCreateCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"create";
    ContainerCreateCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// List Command
struct ContainerListCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"list";
    ContainerListCommand(const std::wstring& parent) : Command(CommandName, {L"ls", L"ps"}, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ValidateArgumentsInternal(const ArgMap& execArgs) const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Run Command
struct ContainerRunCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"run";
    ContainerRunCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Start Command
struct ContainerStartCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"start";
    ContainerStartCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

// Stop Command
struct ContainerStopCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"stop";
    ContainerStopCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc