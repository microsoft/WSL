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
#include "TableOutput.h"
#include "Task.h"

using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
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
        sessionId = SessionOptions::GetDefaultSessionName();
    }

    context.ExitCode = SessionService::Attach(sessionId);
}

void CreateSession(CLIExecutionContext& context)
{
    if (context.Args.Contains(ArgType::Session))
    {
        // If provided session name is not the default CLI session use open only.
        // This also ensures that mixed elevation types will only attempt to open
        // a session and not create it. Example: Admin process attempting to open
        // a non-admin session will fail to create but succeed to open, preventing
        // accidental creation of a non-admin session with admin permissions.
        const auto& sessionName = context.Args.Get<ArgType::Session>();
        if (!SessionOptions::IsDefaultSessionName(sessionName))
        {
            context.Data.Add<Data::Session>(SessionService::OpenSession(sessionName));
            return;
        }
    }

    // Create/open the default session. Create is only called with default session
    // settings so we ensure the CLI sessions are created with correct permissions.
    SessionOptions options{};
    context.Data.Add<Data::Session>(SessionService::CreateSession(options));
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
    else
    {
        sessionId = SessionOptions::GetDefaultSessionName();
    }

    context.ExitCode = SessionService::TerminateSession(sessionId);
}

void EnterSession(CLIExecutionContext& context)
{
    auto storagePath = context.Args.Get<ArgType::StoragePath>();

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
        PrintMessage(sessionName, stderr);
    }

    context.ExitCode = SessionService::Enter(storagePath, sessionName);
}

} // namespace wsl::windows::wslc::task
