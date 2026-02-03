// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "command.h"

namespace wsl::windows::wslc
{
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

    struct ContainerRunCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"run";
        ContainerRunCommand(std::wstring parent) : Command(CommandName, {}, parent, Visibility::Show) {}
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };
}
