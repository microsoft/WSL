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
    WI_ASSERT(context.Args.Contains(ArgType::SessionId));
    SessionService::Attach(context.Args.Get<ArgType::SessionId>());
}

void CreateSession(CLIExecutionContext& context)
{
    std::optional<SessionOptions> options = std::nullopt;
    if (context.Args.Contains(ArgType::Session))
    {
        // TODO: Add session ID to the session options to open the specified session.
    }

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
