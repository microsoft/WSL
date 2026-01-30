/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    Entry point for the wslc CLI.

--*/

#include "precomp.h"
#include "CommandLine.h"
#include "wslutil.h"
#include "wslaservice.h"
#include "WslSecurity.h"
#include "WSLAProcessLauncher.h"
#include "ExecutionContext.h"
#include "ListCommand.h"
#include "ShellCommand.h"
#include "ImageCommand.h"
#include "ContainerCommand.h"
#include "Utils.h"
#include <thread>
#include <format>

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::common::wslutil::WSLAErrorDetails;

static void PrintUsage()
{
    wslutil::PrintMessage(Localization::MessageWslcUsage(), stderr);
}

int wslc_main(std::wstring_view commandLine)
{
    // Initialize runtime and COM.
    wslutil::ConfigureCrt();
    wslutil::InitializeWil();

    WslTraceLoggingInitialize(WslaTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanupTelemetry = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslTraceLoggingUninitialize(); });

    wslutil::SetCrtEncoding(_O_U8TEXT);

    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wslutil::CoInitializeSecurity();

    WSADATA data{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
    auto wsaCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WSACleanup(); });

    // Parse the top-level verb (list, shell, --help).
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 1, true);

    bool help = false;
    std::wstring verb;

    parser.AddPositionalArgument(verb, 0);
    parser.AddArgument(help, L"--help", L'h');

    parser.Parse();

    if (verb == L"list")
    {
        return RunListCommand(commandLine);
    }
    
    if (verb == L"shell")
    {
        return RunShellCommand(commandLine);
    }
    
    if (verb == L"container")
    {
        wslc::commands::ContainerCommand command;
        command.Execute(commandLine, 2);
        return 0;
    }
    
    if (verb == L"image")
    {
        return wslc::commands::RunImageCommand(commandLine);
    }

    wslutil::PrintMessage(Localization::MessageWslaUnknownCommand(verb.c_str()), stderr);
    PrintUsage();

    // Unknown verb - show usage and fail.
    return 1;
}

int wmain(int, wchar_t**)
{
    wsl::windows::common::EnableContextualizedErrors(false);

    ExecutionContext context{Context::WslC};
    int exitCode = 1;
    HRESULT result = S_OK;

    try
    {
        exitCode = wslc_main(GetCommandLineW());
    }
    catch (...)
    {
        result = wil::ResultFromCaughtException();
    }

    if (FAILED(result))
    {
        if (const auto& reported = context.ReportedError())
        {
            auto strings = wsl::windows::common::wslutil::ErrorToString(*reported);
            auto errorMessage = strings.Message.empty() ? strings.Code : strings.Message;
            wslutil::PrintMessage(Localization::MessageErrorCode(errorMessage, wslutil::ErrorCodeToString(result)), stderr);
        }
        else
        {
            // Fallback for errors without context
            wslutil::PrintMessage(Localization::MessageErrorCode("", wslutil::ErrorCodeToString(result)), stderr);
        }
    }

    return exitCode;
}