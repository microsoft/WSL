// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Command.h"

namespace wsl::windows::wslc
{
    struct RootCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"root";

        RootCommand() : Command(CommandName, {}) {}

        std::vector<std::unique_ptr<Command>> GetCommands() const override;
        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        virtual void ExecuteInternal(CLIExecutionContext& context) const;
    };
}