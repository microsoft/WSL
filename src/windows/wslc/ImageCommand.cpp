/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageCommand.cpp

Abstract:
    This file contains the ImageCommand implementation

--*/
#include "precomp.h"
#include "ImageCommand.h"
#include "ImageService.h"
#include "TablePrinter.h"
#include <CommandLine.h>
#include <format>

namespace wsl::windows::wslc::commands {
using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;

int ImagePullCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_image, L"Image name is required.");
    auto session = m_sessionService.CreateSession();

    PullImageCallback callback;
    services::ImageService imageService;
    imageService.Pull(session, m_image, &callback);
    return 0;
}

int ImageListCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    auto session = m_sessionService.CreateSession();
    services::ImageService imageService;
    auto images = imageService.List(session);
    if (m_format == "json")
    {
        for (const models::ImageInformation& image : images)
        {
            wprintf(L"%hs", wsl::shared::ToJson(image).c_str());
        }
    }
    else if (m_quiet)
    {
        for (const auto& image : images)
        {
            wprintf(L"%hs\n", image.Name.c_str());
        }
    }
    else
    {
        utils::TablePrinter tablePrinter({L"NAME", L"SIZE (MB)"});
        for (const auto& image : images)
        {
            tablePrinter.AddRow(
                {wsl::shared::string::MultiByteToWide(image.Name), std::format(L"{:.2f} MB", static_cast<double>(image.Size) / (1024 * 1024))});
        }

        tablePrinter.Print();
    }

    return 0;
}

int ImageCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    if (m_subverb == m_list.Name())
    {
        return m_list.Execute(commandLine, parserOffset + 1);
    }

    if (m_subverb == m_pull.Name())
    {
        return m_pull.Execute(commandLine, parserOffset + 1);
    }

    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_subverb, L"Error: Missing subcommand");
    wslutil::PrintMessage(L"Error: Invalid subcommand specified", stderr);
    PrintHelp();
    return 1;
}

ChangeTerminalMode::ChangeTerminalMode(HANDLE console, bool cursorVisible)
{
    m_console = console;
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleCursorInfo(console, &m_originalCursorInfo));
    CONSOLE_CURSOR_INFO newCursorInfo = m_originalCursorInfo;
    newCursorInfo.bVisible = cursorVisible;
    THROW_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(console, &newCursorInfo));
}

ChangeTerminalMode::~ChangeTerminalMode()
{
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(m_console, &m_originalCursorInfo));
}

auto PullImageCallback::MoveToLine(SHORT line)
{
    if (line > 0)
    {
        wprintf(L"\033[%iA", line);
    }

    return wil::scope_exit([line = line]() {
        if (line > 1)
        {
            wprintf(L"\033[%iB", line - 1);
        }
    });
}

HRESULT PullImageCallback::OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total)
{
    try
    {
        if (id == nullptr || *id == '\0') // Print all 'global' statuses on their own line
        {
            wprintf(L"%hs\n", status);
            m_currentLine++;
            return S_OK;
        }

        auto info = Info();

        auto it = m_statuses.find(id);
        if (it == m_statuses.end())
        {
            // If this is the first time we see this ID, create a new line for it.
            m_statuses.emplace(id, m_currentLine);
            wprintf(L"%ls\n", GenerateStatusLine(status, id, current, total, info).c_str());
            m_currentLine++;
        }
        else
        {
            auto revert = MoveToLine(m_currentLine - it->second);
            wprintf(L"%ls\n", GenerateStatusLine(status, id, current, total, info).c_str());
        }

        return S_OK;
    }
    CATCH_RETURN();
}

CONSOLE_SCREEN_BUFFER_INFO PullImageCallback::Info()
{
    CONSOLE_SCREEN_BUFFER_INFO info{};
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
    return info;
}

std::wstring PullImageCallback::GenerateStatusLine(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total, const CONSOLE_SCREEN_BUFFER_INFO& Info)
{
    std::wstring line;
    if (Total != 0)
    {
        line = std::format(L"{} '{}': {}%", Status, Id, Current * 100 / Total);
    }
    else if (Current != 0)
    {
        line = std::format(L"{} '{}': {}s", Status, Id, Current);
    }
    else
    {
        line = std::format(L"{} '{}'", Status, Id);
    }

    // Erase any previously written char on that line.
    while (line.size() < Info.dwSize.X)
    {
        line += L' ';
    }

    return line;
}
} // namespace wsl::windows::wslc::commands
