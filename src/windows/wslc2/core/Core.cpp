// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "Errors.h"
#include "ExecutionContext.h"
#include "Invocation.h"
#include "RootCommand.h"
#include "TaskBase.h"

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    int CoreMain(int argc, wchar_t const** argv) try
    {
        EnableContextualizedErrors(false);
        CLIExecutionContext context;
        int exitCode = 1;
        HRESULT result = S_OK;

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

        std::unique_ptr<Command> command = std::make_unique<RootCommand>();

        try
        {
            std::vector<std::wstring> args;
            for (int i = 1; i < argc; ++i)
            {
                args.emplace_back(argv[i]);
            }

            // Log the arguments for diagnostic purposes.
            std::wstringstream wstrstr;
            wstrstr << L"WSLC invoked with arguments:";
            for (const auto& arg : args)
            {
                wstrstr << L" '" << arg << L'\'';
            }
        
            // Log the invocation
            ////wslutil::PrintMessage(wstrstr.str(), stdout);

            Invocation invocation{ std::move(args) };
            std::unique_ptr<Command> subCommand = command->FindSubCommand(invocation);
            while (subCommand)
            {
                command = std::move(subCommand);
                subCommand = command->FindSubCommand(invocation);
            }

            command->ParseArguments(invocation, context.Args);
            context.SetExecutingCommand(command.get());
            command->ValidateArguments(context.Args);
        }
        // Exceptions specific to parsing the arguments of a command
        catch (const CommandException& ce)
        {
            command->OutputHelp(&ce);
            return WSLC_CLI_ERROR_INVALID_CL_ARGUMENTS;
        }
        catch (...)
        {
            return task::HandleException(context, std::current_exception());
        }

        return Execute(context, command);
    }
    catch (...)
    {
        return WSLC_CLI_ERROR_INTERNAL_ERROR;
    }
} // namespace wsl::windows::wslc
