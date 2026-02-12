// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "TestCommand.h"
#include "TaskBase.h"
#include "CommonTasks.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    std::vector<Argument> TestCommand::GetArguments() const
    {
        return
        {
            Argument::Create(ArgType::ContainerId, true),
            Argument::Create(ArgType::ForwardArgs),
            Argument::Create(ArgType::Attach),
            Argument::Create(ArgType::Interactive),
            Argument::Create(ArgType::SessionId),
            Argument::Create(ArgType::Publish),
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
        context << task::StoreSessionId;

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

        if (context.Args.Contains(ArgType::Publish))
        {
            for (const auto& port : context.Args.GetAll<ArgType::Publish>())
            {
                PrintMessage(L"  Port: " + port);
            }
        }

        if (context.Args.Contains(ArgType::ForwardArgs))
        {
            auto forwardedArgs = context.Args.Get<ArgType::ForwardArgs>();
            PrintMessage(L"Forwarded " + std::to_wstring(forwardedArgs.size()) + L" Args:");
            for (const auto& arg : forwardedArgs)
            {
                PrintMessage(L"    " + arg);
            }

            std::wstring concatenatedArgs = std::accumulate(
                forwardedArgs.begin(),
                forwardedArgs.end(),
                std::wstring{},
                [](const std::wstring& a, const std::wstring& b) {
                    return a.empty() ? b : a + L" " + b;
                });

            PrintMessage(L"  Concatenated: " + concatenatedArgs);
        }

    }
}
