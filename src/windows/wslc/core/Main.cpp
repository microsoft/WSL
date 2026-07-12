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
#include "EnvironmentOptions.h"
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
    HRESULT result = S_OK;

    wslutil::ConfigureCrt();
    wslutil::InitializeWil();

    WslTraceLoggingInitialize(WslcTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanupTelemetry = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslTraceLoggingUninitialize(); });

    wslutil::SetCrtEncoding(_O_U8TEXT);
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wslutil::CoInitializeSecurity();

    // Must be declared after COM init; it holds COM references.
    CLIExecutionContext context;

    // SetConsoleCtrlHandler only accepts plain function pointers, so route Ctrl-C
    // through a static reference into the context.
    static auto& s_cancelEvent = context.CancelEvent;
    auto ctrlHandler = [](DWORD ctrlType) -> BOOL {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
        {
            if (s_cancelEvent && !s_cancelEvent.is_signaled())
            {
                s_cancelEvent.SetEvent();
                return TRUE;
            }
        }
        return FALSE;
    };
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    auto unregisterHandler = wil::scope_exit([&]() { SetConsoleCtrlHandler(ctrlHandler, FALSE); });

    WSADATA data{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
    auto wsaCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WSACleanup(); });

    std::unique_ptr<Command> command = std::make_unique<RootCommand>();

    // Environment variable scanning.
    // The env-bound argument set is the only state needed before NO_COLOR is
    // applied; keep just this and the noexcept env apply outside the try so a
    // throw can't reroute through the colored-help error path.
    auto envDefs = command->GetGlobalsAndEnvArguments();
    ApplyEnvironmentOptions(context.GlobalArgs, envDefs);
    context.ApplyGlobalOptions();

    // Past this point, environment variable options are in effect.

    try
    {
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }

        Invocation invocation{std::move(args)};

        // Pass 1 — CLI globals. Consume only the global options we recognize at
        // the front of the invocation; anything else (subcommands, unknown
        // options, --help, --version, malformed tokens) is left in place for
        // the regular pipeline to parse and report against the right command.
        auto cliGlobals = command->GetGlobalArguments();
        command->ParseArguments(
            invocation,
            context.GlobalArgs,
            cliGlobals,
            /*optionsOnly*/ true,
            /*stopOnUnknown*/ true,
            /*overridableDefaults*/ envDefs);
        command->ValidateArguments(context.GlobalArgs, envDefs, /*runInternalHook*/ false);
        context.ApplyGlobalOptions();

        // Past this point, global options are in effect.

        // Pass 2 - Subcommand and leaf command resolution.
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
    catch (const CommandException& ce)
    {
        // Input failure: show help alongside the error so the user can correct it.
        command->OutputHelp(&ce);
        return 1;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();

        // If the user pressed Ctrl-C, acknowledge the cancellation and exit.
        if (context.CancelEvent && context.CancelEvent.is_signaled())
        {
            // Cancel events are often considered warnings rather than errors, as the user
            // intentionally triggered it.
            const auto strings = wslutil::ErrorToString({.Code = HRESULT_FROM_WIN32(ERROR_CANCELLED)});
            context.Reporter.Warn(L"\n{}\n", strings.Message);

            // Exit with code 1 is consistent with Docker build and pull, but the POSIX-convention
            // for cancellation is exit code 130, which is used by Docker compose and most shells.
            // TODO: Consider switching to 130 or differentiate the cancellation types when we have
            // more than image cancellation supported.
            return 1;
        }

        // Using WSL shared utility to get the HRESULT from the caught exception.
        // CLIExecutionContext is a derived class of wsl::windows::common::ExecutionContext.
        result = wil::ResultFromCaughtException();

        if (FAILED(result))
        {
            if (const auto& reported = context.ReportedError())
            {
                auto strings = wslutil::ErrorToString(*reported);
                auto errorMessage = strings.Message.empty() ? strings.Code : strings.Message;
                context.Reporter.Error(L"{}\n", Localization::MessageErrorCode(errorMessage, wslutil::ErrorCodeToString(result)));
            }
            else
            {
                // Fallback for errors without context
                context.Reporter.Error(L"{}\n", Localization::MessageErrorCode(L"", wslutil::ErrorCodeToString(result)));
            }
        }
    }

    if (context.ExitCode.has_value())
    {
        return context.ExitCode.value();
    }

    return FAILED(result) ? 1 : 0;
}
catch (...)
{
    return 1;
}
} // namespace wsl::windows::wslc

int wmain(int argc, wchar_t const** argv)
{
    return wsl::windows::wslc::CoreMain(argc, argv);
}
