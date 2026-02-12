/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ICommand.h

Abstract:

    This file contains the ICommand definition

--*/
#pragma once

#include <CommandLine.h>
#include "SessionService.h"

#define CMD_IF_HELP_PRINT_HELP() \
    if (m_help) \
    { \
        PrintHelp(); \
        return 0; \
    }
#define CMD_ARG_REQUIRED(arg, msg) \
    if (arg.empty()) \
    { \
        wsl::windows::common::wslutil::PrintMessage(msg, stderr); \
        PrintHelp(); \
        return E_INVALIDARG; \
    }
#define CMD_ARG_ARRAY_REQUIRED(argArray, msg) \
    if (argArray.empty()) \
    { \
        wsl::windows::common::wslutil::PrintMessage(msg, stderr); \
        PrintHelp(); \
        return E_INVALIDARG; \
    }

namespace wslc::commands {
class ICommand
{
public:
    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual std::vector<std::string> Options() const
    {
        return {};
    };
    virtual void LoadArguments(wsl::shared::ArgumentParser& parser) {};
    std::vector<std::string> Arguments(int startIndex = 0)
    {
        return {m_arguments.begin() + startIndex, m_arguments.end()};
    }
    std::string GetFullDescription() const;
    std::string GetShortDescription() const;
    void PrintHelp() const;
    int Execute(std::wstring_view commandLine, int parserOffset = 0)
    {
        m_help = false;
        m_arguments.clear();
        wsl::shared::ArgumentParser parser(std::wstring{commandLine}, L"wslc", parserOffset, true);
        parser.AddArgument(m_help, L"--help", 'h');
        LoadArguments(parser);
        parser.Parse();
        for (size_t i = parser.ParseIndex(); i < parser.Argc(); i++)
        {
            m_arguments.push_back(wsl::shared::string::WideToMultiByte(parser.Argv(i)));
        }
        return ExecuteInternal(commandLine, parserOffset);
    }

protected:
    virtual int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) = 0;
    bool m_help{};
    wslc::services::SessionService m_sessionService;

private:
    std::vector<std::string> m_arguments;
};
} // namespace wslc::commands
