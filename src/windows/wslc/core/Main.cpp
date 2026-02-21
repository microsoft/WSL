/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Main.cpp

Abstract:

    Main program entry point.

--*/
#define WIN32_LEAN_AND_MEAN
#pragma once
#include <Windows.h>
#include "precomp.h"
#include "wslutil.h"
#include "Errors.h"
#include "CLIExecutionContext.h"
#include "Invocation.h"
#include "RootCommand.h"

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc {
int CoreMain(int argc, wchar_t const** argv)
try
{
    EnableContextualizedErrors(false, true);
    CLIExecutionContext context;
    HRESULT result = S_OK;

    // Initialize runtime and COM.
    wslutil::ConfigureCrt();
    wslutil::InitializeWil();

    WslTraceLoggingInitialize(WslaTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanupTelemetry = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslTraceLoggingUninitialize(); });

    wsl::windows::common::wslutil::ConfigureCrashHandler();

    wslutil::SetCrtEncoding(_O_U8TEXT);
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wslutil::CoInitializeSecurity();

    WSADATA data{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
    auto wsaCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WSACleanup(); });

    std::unique_ptr<Command> command = std::make_unique<RootCommand>();

    try
    {
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }

        Invocation invocation{std::move(args)};
        std::unique_ptr<Command> subCommand = command->FindSubCommand(invocation);
        while (subCommand)
        {
            command = std::move(subCommand);
            subCommand = command->FindSubCommand(invocation);
        }

        command->ParseArguments(invocation, context.Args);
        command->ValidateArguments(context.Args);
        command->Execute(context);
    }
    // Exceptions specific to parsing the arguments of a command
    catch (const CommandException& ce)
    {
        // A command exception means there was an input failure. Display the help
        // along with the error message to help the user correct their input.
        command->OutputHelp(&ce);
        return E_INVALIDARG;
    }
    // Any other type of error unrelated to the command parsing.
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();

        // Using WSL shared utility to get the HRESULT from the caught exception.
        // CLIExecutionContext is a derived class of wsl::windows::common::ExecutionContext.
        result = wil::ResultFromCaughtException();
        if (FAILED(result))
        {
            if (const auto& reported = context.ReportedError())
            {
                auto strings = wslutil::ErrorToString(*reported);
                auto errorMessage = strings.Message.empty() ? strings.Code : strings.Message;
                wslutil::PrintMessage(Localization::MessageErrorCode(errorMessage, wslutil::ErrorCodeToString(result)), stderr);
            }
            else
            {
                // Fallback for errors without context
                wslutil::PrintMessage(Localization::MessageErrorCode("", wslutil::ErrorCodeToString(result)), stderr);
            }
        }
    }

    return result;
}
catch (...)
{
    return E_UNEXPECTED;
}
} // namespace wsl::windows::wslc

int wmain(int argc, wchar_t const** argv)
{
    return wsl::windows::wslc::CoreMain(argc, argv);
}
