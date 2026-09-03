/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionTasks.cpp

Abstract:

    Implementation of session command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentConvertedTypes.h"
#include "CLIExecutionContext.h"
#include "Exceptions.h"
#include "SessionService.h"
#include "SessionTasks.h"
#include "TableOutput.h"
#include "Task.h"

using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

void AttachToSession(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    context.ExitCode = SessionService::Attach(context.Terminal, session);
}

void OpenSessionIfSpecified(CLIExecutionContext& context)
{
    if (context.GlobalArgs.Contains(ArgType::Session))
    {
        const auto& sessionName = context.GlobalArgs.GetValue<ArgType::Session>();
        try
        {
            context.Data.Add<Data::Session>(SessionService::OpenSession(sessionName));
        }
        catch (const wil::ResultException& ex)
        {
            if (ex.GetErrorCode() == WSLC_E_SESSION_NOT_FOUND &&
                context.GlobalArgs.GetSource(ArgType::Session) == argument::ArgumentValueSource::Environment)
            {
                throw ExecutionException(Localization::MessageWslcEnvironmentSessionNotFound(sessionName));
            }

            throw;
        }
    }
}

void OpenOrCreateDefaultSession(CLIExecutionContext& context)
{
    if (!context.Data.Contains(Data::Session))
    {
        context.Data.Add<Data::Session>(SessionService::OpenOrCreateDefaultSession(context.Terminal));
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
    if (context.Args.GetValue<ArgType::Verbose>())
    {
        const wchar_t* plural = sessions.size() == 1 ? L"" : L"s";
        context.Terminal.Output(L"[wslc] Found {} session{}\n", sessions.size(), plural);
    }

    TableOutput<3> table(
        context.Terminal,
        {Localization::MessageWslcHeaderId(), Localization::MessageWslcHeaderCreatorPid(), Localization::MessageWslcHeaderDisplayName()});

    for (const auto& session : sessions)
    {
        table.WriteRow({
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
    context.ExitCode = SessionService::TerminateSession(context.Terminal, session);
}

void RunInSession(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();

    std::vector<std::string> arguments;
    arguments.emplace_back(wsl::windows::common::string::WideToMultiByte(context.Args.GetValue<ArgType::Command>()));
    if (context.Args.Contains(ArgType::ForwardArgs))
    {
        for (const auto& arg : context.Args.GetValue<ArgType::ForwardArgs>())
        {
            arguments.emplace_back(wsl::windows::common::string::WideToMultiByte(arg));
        }
    }

    context.ExitCode = SessionService::Run(context.Terminal, session, arguments);
}

void EnterSession(CLIExecutionContext& context)
{
    auto storagePath = std::filesystem::absolute(context.Args.GetValue<ArgType::StoragePath>());

    std::wstring sessionName;
    if (context.Args.Contains(ArgType::Name))
    {
        sessionName = context.Args.GetValue<ArgType::Name>();
    }
    else
    {
        GUID guid{};
        THROW_IF_FAILED(CoCreateGuid(&guid));
        sessionName = wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
    }

    context.ExitCode = SessionService::Enter(context.Terminal, storagePath.wstring(), sessionName);
}

} // namespace wsl::windows::wslc::task
