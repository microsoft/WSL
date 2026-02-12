// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Command.h"

namespace wsl::windows::wslc
{
    struct TestCommand final : public Command
    {
        constexpr static std::wstring_view CommandName = L"test";

        TestCommand(std::wstring parent) : Command(CommandName, parent, Visibility::Show) {}

        std::vector<Argument> GetArguments() const override;
        std::wstring_view ShortDescription() const override;
        std::wstring_view LongDescription() const override;

    protected:
        void ExecuteInternal(CLIExecutionContext& context) const override;
    };
}
