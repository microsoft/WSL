/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ShellCommand.cpp

Abstract:

    This file contains the ShellCommand implementation

--*/
#include "precomp.h"
#include "Utils.h"
#include "ShellCommand.h"
#include "ShellService.h"
#include "TablePrinter.h"

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::WSLAProcessLauncher;

namespace wslc::commands {
int ShellListCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    wslc::services::ShellService shellService;
    auto sessions = shellService.List();
    if (m_verbose)
    {
        const wchar_t* plural = sessions.size() == 1 ? L"" : L"s";
        wslutil::PrintMessage(std::format(L"[wslc] Found {} session{}", sessions.size(), plural), stdout);
    }

    TablePrinter tablePrinter({L"ID", L"Creator PID", L"Display Name"});
    for (const auto& session : sessions)
    {
        tablePrinter.AddRow({
            std::to_wstring(session.SessionId),
            std::to_wstring(session.CreatorPid),
            std::wstring(session.DisplayName.begin(), session.DisplayName.end()),
        });
    }
    tablePrinter.Print();
    return 0;
}

int ShellAttachCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_name, L"Error: Session name is required to attach.");
    wslc::services::ShellService shellService;
    return shellService.Attach(wsl::shared::string::MultiByteToWide(m_name));
}

int ShellCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    if (m_subverb == m_list.Name())
    {
        return m_list.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_attach.Name())
    {
        return m_attach.Execute(commandLine, parserOffset + 1);
    }

    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_subverb, L"Error: Invalid or missing subcommand.");
    return 0;
}
}
