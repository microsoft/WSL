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

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;

int wsladiag_main(std::wstring_view commandLine)
{
    //
    // Standard process initialization (matches other WSL tools)
    //
    wslutil::ConfigureCrt();
    wslutil::InitializeWil();

    WslTraceLoggingInitialize(LxssTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanupTelemetry = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []()
    {
        WslTraceLoggingUninitialize();
    });

    wslutil::SetCrtEncoding(_O_U8TEXT);

    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wslutil::CoInitializeSecurity();

    WSADATA data{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
    auto wsaCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []()
    {
        WSACleanup();
    });

    //
    // Command-line parsing using ArgumentParser
    //
    ArgumentParser parser(std::wstring{commandLine}, L"wsladiag");

    bool help = false;
    bool list = false;

    parser.AddArgument(list, L"--list");
    parser.AddArgument(help, L"--help", L'h'); //  short option is a single wide char 
    parser.Parse();

    auto printUsage = []()
    {
        wslutil::PrintMessage(
            L"wsladiag - WSLA diagnostics tool\n"
            L"Usage:\n"
            L"  wsladiag --list    List WSLA sessions\n"
            L"  wsladiag --help    Show this help",
            stdout);
    };

    // Slightly clearer help logic
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

    // --list
    wslutil::PrintMessage(
        L"[wsladiag] --list: placeholder.\n"
        L"Next step: call WSLA service ListSessions and display sessions.",
        stdout);
    // TODO: call WSLA service COM interface to retrieve and display sessions.
    return 0;
}

int wmain(int /*argc*/, wchar_t** /*argv*/)
{
    try
    {
        // Use the full command line so ArgumentParser sees the raw string
        return wsladiag_main(GetCommandLineW());
    }
    CATCH_RETURN();
}
