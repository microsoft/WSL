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
#include "ExecutionContext.h"
#include "ShellCommand.h"
#include "ImageCommand.h"
#include "ContainerCommand.h"
#include "Utils.h"
#include <thread>
#include <format>

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

namespace wslc::commands {
// wslc image
class RootCommand : public ICommand
{
public:
    std::string Name() const override { return ""; }
    std::string Description() const override { return "wslc root command"; }
    std::vector<std::string> Options() const override
    {
        return {
            m_image.GetShortDescription(),
            m_container.GetShortDescription(),
            m_shell.GetShortDescription(),
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_subverb}, 0);
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override
    {
        if (m_subverb == m_image.Name())
        {
            return m_image.Execute(commandLine, parserOffset + 1);
        }

        if (m_subverb == m_container.Name())
        {
            return m_container.Execute(commandLine, parserOffset + 1);
        }

        if (m_subverb == m_shell.Name())
        {
            return m_shell.Execute(commandLine, parserOffset + 1);
        }

        CMD_IF_HELP_PRINT_HELP();
        CMD_ARG_REQUIRED(m_subverb, L"Error: Invalid or missing subcommand.");
        PrintHelp();
        return 0;
    }

private:
    std::string m_subverb;
    ImageCommand m_image;
    ContainerCommand m_container;
    ShellCommand m_shell;
};
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
    std::wstring verb;
    parser.AddPositionalArgument(verb, 0);
    parser.Parse();

    wslc::commands::RootCommand rootCommand;
    return rootCommand.Execute(commandLine, 1);
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