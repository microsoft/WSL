// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "TestCommand.h"
#include "TaskBase.h"
#include "CommonTasks.h"
#include "TestTasks.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    std::vector<Argument> TestCommand::GetArguments() const
    {
        return
        {
            Argument::ForType(ArgType::TestArg),
            Argument::ForType(ArgType::ContainerId),
            Argument::ForType(ArgType::ForwardArgs),
            Argument::ForType(ArgType::Attach),
            Argument::ForType(ArgType::Interactive),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::Port),
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
        if (context.Args.Contains(ArgType::TestArg))
        {
            context << task::OutputNinjaCat;
            return;
        }

        if (context.Args.Contains(ArgType::ContainerId))
        {
            PrintMessage(L"Container Id(s):");
            for (const auto& containerId : context.Args.GetAll<ArgType::ContainerId>())
            {
                PrintMessage(L"  Container Id: " + containerId);
            }
        }

        if (context.Args.Contains(ArgType::Interactive))
        {
            PrintMessage(L"  Interactive mode");
        }

        if (context.Args.Contains(ArgType::Attach))
        {
            PrintMessage(L"  Attach to stdout/stderr");
        }

        if (context.Data.Contains(Data::SessionId))
        {
            PrintMessage(L"  Stored SessionId: " + context.Data.Get<Data::SessionId>());
        }

        if (context.Args.Contains(ArgType::Port))
        {
            for (const auto& port : context.Args.GetAll<ArgType::Port>())
            {
                PrintMessage(L"  Port: " + port);
            }
        }

        if (context.Args.Contains(ArgType::ForwardArgs))
        {
            auto forwardedArgs = context.Args.Get<ArgType::ForwardArgs>();
            PrintMessage(L"  Forwarded " + std::to_wstring(forwardedArgs.size()) + L" args:");
            for (const auto& forwardArg : forwardedArgs)
            {
                PrintMessage(L"  Forward Arg: " + forwardArg);
            }
        }

    }
}
