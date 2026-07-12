/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionTasks.cpp

Abstract:

    Implementation of session command related execution logic.

--*/
#include "Argument.h"
#include "CLIExecutionContext.h"
#include "SessionService.h"
#include "SessionTasks.h"
#include "TableOutput.h"
#include "Task.h"

using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

void AttachToSession(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    context.ExitCode = SessionService::Attach(session);
}

void OpenSessionIfSpecified(CLIExecutionContext& context)
{
    if (context.GlobalArgs.Contains(ArgType::Session))
    {
        const auto& sessionName = context.GlobalArgs.Get<ArgType::Session>();
        context.Data.Add<Data::Session>(SessionService::OpenSession(sessionName));
    }
}

void OpenOrCreateDefaultSession(CLIExecutionContext& context)
{
    if (!context.Data.Contains(Data::Session))
    {
        context.Data.Add<Data::Session>(SessionService::OpenOrCreateDefaultSession());
    }
}

void OpenDefaultSession(CLIExecutionContext& context)
{
    if (!context.Data.Contains(Data::Session))
    {
        context.Data.Add<Data::Session>(SessionService::OpenDefaultSession());
    }
}

void ResolveSession(CLIExecutionContext& context)
{
    OpenSessionIfSpecified(context);
    OpenOrCreateDefaultSession(context);
}

void ListSessions(CLIExecutionContext& context)
{
    auto sessions = SessionService::List();
    if (context.Args.Contains(ArgType::Verbose))
    {
        const wchar_t* plural = sessions.size() == 1 ? L"" : L"s";
        PrintMessage(std::format(L"[wslc] Found {} session{}", sessions.size(), plural), stdout);
    }

    TableOutput<3> table(
        {Localization::MessageWslcHeaderId(), Localization::MessageWslcHeaderCreatorPid(), Localization::MessageWslcHeaderDisplayName()});

    for (const auto& session : sessions)
    {
        table.OutputLine({
            std::to_wstring(session.SessionId),
            std::to_wstring(session.CreatorPid),
            session.DisplayName,
        });
    }

    table.Complete();
}

void TerminateSession(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    context.ExitCode = SessionService::TerminateSession(session);
}

void RunInSession(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();

    std::vector<std::string> arguments;
    arguments.emplace_back(wsl::windows::common::string::WideToMultiByte(context.Args.Get<ArgType::Command>()));
    if (context.Args.Contains(ArgType::ForwardArgs))
    {
        for (const auto& arg : context.Args.Get<ArgType::ForwardArgs>())
        {
            arguments.emplace_back(wsl::windows::common::string::WideToMultiByte(arg));
        }
    }

    context.ExitCode = SessionService::Run(session, arguments);
}

void EnterSession(CLIExecutionContext& context)
{
    auto storagePath = std::filesystem::absolute(context.Args.Get<ArgType::StoragePath>());

    std::wstring sessionName;
    if (context.Args.Contains(ArgType::Name))
    {
        sessionName = context.Args.Get<ArgType::Name>();
    }
    else
    {
        GUID guid{};
        THROW_IF_FAILED(CoCreateGuid(&guid));
        sessionName = wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
    }

    context.ExitCode = SessionService::Enter(storagePath.wstring(), sessionName);
}

} // namespace wsl::windows::wslc::task
