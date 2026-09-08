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
#include "JsonUtils.h"
#include "SessionService.h"
#include "SessionTasks.h"
#include "TableOutput.h"
#include "Task.h"
#include "WSLCUserSettings.h"

using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

static void WriteSessionTable(Terminal& terminal, const std::vector<SessionInformation>& sessions)
{
    TableOutput<3> table(
        terminal,
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
        context.Data.Add<Data::Session>(SessionService::OpenSession(sessionName));
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

    WriteSessionTable(context.Terminal, sessions);
}

static std::wstring FormatManagerVersion(const WSLCVersion& version)
{
    return std::format(L"{}.{}.{}", version.Major, version.Minor, version.Revision);
}

void ShowSystemInfo(CLIExecutionContext& context)
{
    const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersionString();
    const auto settingsFilePath = settings::User().SettingsFilePath().wstring();

    switch (context.Args.GetValue<ArgType::Format>(FormatType::Table))
    {
    case FormatType::Json:
    {
        // A JSON document can't be emitted partially, so an unreachable service fails the whole command.
        const auto managerVersionText = FormatManagerVersion(SessionService::ManagerVersion());
        const auto sessions = SessionService::List();

        nlohmann::json root;

        auto& client = root["Client"];
        client["Version"] = std::string{WSL_PACKAGE_VERSION};
        client["KernelVersion"] = std::string{KERNEL_VERSION};
        client["Direct3DVersion"] = std::string{DIRECT3D_VERSION};
        client["DxCoreVersion"] = std::string{DXCORE_VERSION};
        client["WindowsVersion"] = windowsVersion;
        client["SettingsFile"] = settingsFilePath;

        if constexpr (!wsl::shared::OfficialBuild)
        {
            client["MsBuildVersion"] = _MSC_VER;
            client["Commit"] = std::string{COMMIT_HASH};
            client["BuildTime"] = std::string{__TIME__ " " __DATE__};
        }

        auto& server = root["Server"];
        server["SessionManagerVersion"] = managerVersionText;

        auto& sessionArray = server["Sessions"];
        sessionArray = nlohmann::json::array();
        for (const auto& session : sessions)
        {
            sessionArray.push_back({{"ID", session.SessionId}, {"CreatorPid", session.CreatorPid}, {"Name", session.DisplayName}});
        }

        context.Terminal.Output(L"{}\n", ToJsonW(root, c_jsonCompactIndent));
        break;
    }
    case FormatType::Table:
    {
        const auto managerVersionText = FormatManagerVersion(SessionService::ManagerVersion());
        const auto sessions = SessionService::List();

        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_SystemInfoClientHeader());
        context.Terminal.Output(
            L"{}\n", Localization::WSLCCLI_SystemInfoVersions(WSL_PACKAGE_VERSION, KERNEL_VERSION, DIRECT3D_VERSION, DXCORE_VERSION, windowsVersion));

        if constexpr (!wsl::shared::OfficialBuild)
        {
            context.Terminal.Output(L"{}\n", Localization::MessageBuildInfo(_MSC_VER, COMMIT_HASH, __TIME__ " " __DATE__));
        }

        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_SystemInfoSettingsFile(settingsFilePath));

        context.Terminal.Output(L"\n{}\n", Localization::WSLCCLI_SystemInfoServerHeader());
        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_SystemInfoSessionManagerVersion(managerVersionText));
        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_SystemInfoSessions(sessions.size()));

        if (!sessions.empty())
        {
            WriteSessionTable(context.Terminal, sessions);
        }

        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
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
