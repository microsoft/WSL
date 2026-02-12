// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Command.h"

namespace wsl::windows::wslc
{
    // Root Container Command
    struct ContainerCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"container";
        ContainerCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

        std::vector<std::unique_ptr<Command>> GetCommands() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Create Command
    struct ContainerCreateCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"create";
        ContainerCreateCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Delete Command
    struct ContainerDeleteCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"delete";
        ContainerDeleteCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Exec Command
    struct ContainerExecCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"exec";
        ContainerExecCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Inspect Command
    struct ContainerInspectCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"inspect";
        ContainerInspectCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Kill Command
    struct ContainerKillCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"kill";
        ContainerKillCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // List Command
    struct ContainerListCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"list";
        ContainerListCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Run Command
    struct ContainerRunCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"run";
        ContainerRunCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ValidateArgumentsInternal(Args& execArgs) const override;
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Start Command
    struct ContainerStartCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"start";
        ContainerStartCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    // Stop Command
    struct ContainerStopCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"stop";
        ContainerStopCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };
}
