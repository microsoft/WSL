// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "TestCommand.h"
#include "WorkflowBase.h"
#include "TestFlow.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    std::vector<Argument> TestCommand::GetArguments() const
    {
        return
        {
            Argument::ForType(Args::Type::TestArg),
        };
    }

    std::wstring_view TestCommand::ShortDescription() const
    {
        return { L"Test command" };
    }

    std::wstring_view TestCommand::LongDescription() const
    {
        return { L"Test command for demonstration purposes." };
    }

    void TestCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        context << workflow::OutputNinjaCat;
    }
}
