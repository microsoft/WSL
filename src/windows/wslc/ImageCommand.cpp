/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageCommand.cpp

Abstract:
    This file contains the ImageCommand implementation

--*/
#include "precomp.h"
#include "ImageCommand.h"
#include "Utils.h"
#include <CommandLine.h>
#include <format>
#include "ImageService.h"
#include "TablePrinter.h"

namespace wslc::commands
{
using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;

int ImagePullCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    CMD_ARG_REQUIRED(m_image, L"Image name is required.");
    auto session = m_sessionService.CreateSession();
    PullImpl(*session.Get(), m_image);
    return 0;
}

int ImageListCommand::ExecuteInternal(std::wstring_view commandLine, int parserOffset)
{
    CMD_IF_HELP_PRINT_HELP();
    auto session = m_sessionService.CreateSession();
    wslc::services::ImageService imageServie;
    auto images = imageServie.List(session);
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
        TablePrinter tablePrinter({L"NAME", L"SIZE (MB)"});
        for (const auto& [imageName, size] : images)
        {
            tablePrinter.AddRow({
                std::wstring(imageName.begin(), imageName.end()), 
                std::format(L"{:.2f} MB", static_cast<double>(size) / (1024 * 1024))
            });
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
    CMD_ARG_REQUIRED(m_subverb, L"Error: Invalid or missing subcommand.");
    return 0;
}}
