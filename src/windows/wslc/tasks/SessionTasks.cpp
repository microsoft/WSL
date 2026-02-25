/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionTasks.cpp

Abstract:

    Implementation of session command related execution logic.

--*/
#include "Argument.h"
#include "CLIExecutionContext.h"
#include "Task.h"
#include "TablePrinter.h"
#include "SessionTasks.h"
#include "ShellService.h"

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {
void ListSessions(CLIExecutionContext& context)
{
    auto sessions = ShellService::List();
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

void AttachToSession(CLIExecutionContext& context)
{
    WI_ASSERT(context.Args.Contains(ArgType::SessionId));
    ShellService::Attach(context.Args.Get<ArgType::SessionId>());
}
} // namespace wsl::windows::wslc::task
