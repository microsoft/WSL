/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionTasks.cpp

Abstract:

    Implementation of session command related execution logic.

--*/
#include "Argument.h"
#include "CLIExecutionContext.h"
#include "SessionModel.h"
#include "SessionService.h"
#include "SessionTasks.h"
#include "TablePrinter.h"
#include "Task.h"

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;
using wsl::windows::wslc::models::SessionOptions;

namespace wsl::windows::wslc::task {

void AttachToSession(CLIExecutionContext& context)
{
    std::wstring sessionId;
    if (context.Args.Contains(ArgType::SessionId))
    {
        sessionId = context.Args.Get<ArgType::SessionId>();
    }
    else
    {
        sessionId = SessionOptions::s_defaultSessionName;
    }

    context.ExitCode = SessionService::Attach(sessionId);
}

void CreateSession(CLIExecutionContext& context)
{
    if (context.Args.Contains(ArgType::Session))
    {
        // If provided session name is not the default CLI session, open that one.
        const auto& sessionName = context.Args.Get<ArgType::Session>();
        if (!wsl::shared::string::IsEqual(sessionName, SessionOptions::s_defaultSessionName))
        {
            context.Data.Add<Data::Session>(SessionService::OpenSession(sessionName));
            return;
        }
    }

    // Create/open the CLI session.
    SessionOptions options = SessionOptions::Default();
    context.Data.Add<Data::Session>(SessionService::CreateSession(options));
}

void ListSessions(CLIExecutionContext& context)
{
    auto sessions = SessionService::List();
    if (context.Args.Contains(ArgType::Verbose))
    {
        const wchar_t* plural = sessions.size() == 1 ? L"" : L"s";
        wslutil::PrintMessage(std::format(L"[wslc] Found {} session{}", sessions.size(), plural), stdout);
    }

    utils::TablePrinter tablePrinter(
        {Localization::MessageWslaHeaderId(), Localization::MessageWslaHeaderCreatorPid(), Localization::MessageWslaHeaderDisplayName()});
    for (const auto& session : sessions)
    {
        tablePrinter.AddRow({
            std::to_wstring(session.SessionId),
            std::to_wstring(session.CreatorPid),
            session.DisplayName,
        });
    }
    tablePrinter.Print();
}
} // namespace wsl::windows::wslc::task
