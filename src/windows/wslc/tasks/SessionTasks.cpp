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
    std::wstring sessionId;
    if (context.Args.Contains(ArgType::SessionId))
    {
        sessionId = context.Args.Get<ArgType::SessionId>();
    }

    context.ExitCode = SessionService::Attach(sessionId);
}

void CreateSession(CLIExecutionContext& context)
{
    if (context.Args.Contains(ArgType::Session))
    {
        // User specified a session name — open only, don't create.
        const auto& sessionName = context.Args.Get<ArgType::Session>();
        context.Data.Add<Data::Session>(SessionService::OpenSession(sessionName));
        return;
    }

    // Create/open the default session.
    context.Data.Add<Data::Session>(SessionService::CreateDefaultSession());
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
    std::wstring sessionId;
    if (context.Args.Contains(ArgType::SessionId))
    {
        sessionId = context.Args.Get<ArgType::SessionId>();
    }

    context.ExitCode = SessionService::TerminateSession(sessionId);
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
