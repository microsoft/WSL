/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    Entry point for the wsladiag tool, performs WSL runtime initialization and parses --list/--help.

--*/

#include "precomp.h"
#include "CommandLine.h"
#include "wslutil.h"
#include "wslaservice.h"
#include "WslSecurity.h"

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;

int wsladiag_main(std::wstring_view commandLine)
{
    wslutil::ConfigureCrt();
    wslutil::InitializeWil();

    WslTraceLoggingInitialize(LxssTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanupTelemetry = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslTraceLoggingUninitialize(); });

    wslutil::SetCrtEncoding(_O_U8TEXT);

    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wslutil::CoInitializeSecurity();

    WSADATA data{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
    auto wsaCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WSACleanup(); });

    // Command-line parsing using ArgumentParser.
    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag");

    bool help = false;
    bool list = false;

    parser.AddArgument(list, L"--list");
    parser.AddArgument(help, L"--help", L'h'); //  short option is a single wide char
    parser.Parse();

    auto printUsage = []() {
        wslutil::PrintMessage(
            L"wsladiag - WSLA diagnostics tool\n"
            L"Usage:\n"
            L"  wsladiag --list    List WSLA sessions\n"
            L"  wsladiag --help    Show this help",
            stderr);
    };

    // If '--help' was requested, print usage and exit.
    if (help)
    {
        printUsage();
        return 0;
    }

    if (!list)
    {
        // No recognized command → show usage
        printUsage();
        return 0;
    }

    // --list: Call WSLA service COM interface to retrieve and display sessions.
    try
    {
        wil::com_ptr<IWSLAUserSession> userSession;
        THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLAUserSession), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&userSession)));

        wsl::windows::common::security::ConfigureForCOMImpersonation(userSession.get());

        WSLA_SESSION_INFORMATION* sessions = nullptr;
        ULONG sessionCount = 0;

        THROW_IF_FAILED(userSession->ListSessions(&sessions, &sessionCount));

        auto cleanupSessions = wil::scope_exit([&]() {
            if (sessions != nullptr)
            {
                // Free the CoTaskMem-allocated memory
                for (ULONG i = 0; i < sessionCount; i++)
                {
                    if (sessions[i].DisplayName != nullptr)
                    {
                        CoTaskMemFree(sessions[i].DisplayName);
                    }
                }
                CoTaskMemFree(sessions);
            }
        });

        if (sessionCount == 0)
        {
            wslutil::PrintMessage(L"No WSLA sessions found.\n", stdout);
        }
        else
        {
            wslutil::PrintMessage(std::format(L"Found {} WSLA session{}:\n", sessionCount, sessionCount > 1 ? L"s" : L""), stdout);

            wslutil::PrintMessage(L"ID\tCreator PID\tDisplay Name\n", stdout);
            wslutil::PrintMessage(L"--\t-----------\t------------\n", stdout);

            for (ULONG i = 0; i < sessionCount; i++)
            {
                const auto& session = sessions[i];

                            std::wstring displayName = L"<unnamed>";
                if (session.DisplayName != nullptr)
                {
                    const int length = MultiByteToWideChar(CP_UTF8, 0, session.DisplayName, -1, nullptr, 0);
                    if (length > 0)
                    {
                        displayName.resize(length - 1);
                        MultiByteToWideChar(CP_UTF8, 0, session.DisplayName, -1, displayName.data(), length);
                    }
                }

                wslutil::PrintMessage(std::format(L"{}\t{}\t\t{}\n", session.Id, session.CreatorPid, displayName), stdout);
            }
        }

        return 0;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        wslutil::PrintMessage(std::format(L"Error listing WSLA sessions: 0x{:08x}\n", static_cast<unsigned int>(hr)), stderr);
        return 1;
    }
}

int wmain(int /*argc*/, wchar_t** /*argv*/)
{
    try
    {
        // Use raw Unicode command line so ArgumentParser gets original input.
        return wsladiag_main(GetCommandLineW());
    }
    CATCH_RETURN();
}
