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
        ContainerCommand(std::wstring parent) : Command(CommandName, {}, parent, Visibility::Show) {}
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
        ContainerCreateCommand(std::wstring parent) : Command(CommandName, {}, parent, Visibility::Show) {}
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
        ContainerKillCommand(std::wstring parent) : Command(CommandName, {}, parent, Visibility::Show) {}
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
        ContainerRunCommand(std::wstring parent) : Command(CommandName, {}, parent, Visibility::Show) {}
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
        ContainerStartCommand(std::wstring parent) : Command(CommandName, {}, parent, Visibility::Show) {}
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
        ContainerStopCommand(std::wstring parent) : Command(CommandName, {}, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };

    }
