/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HandleConsoleProgressBar.cpp

Abstract:

    This file contains the HandleConsoleProgressBar class implementation.

--*/

#include "precomp.h"

#include "HandleConsoleProgressBar.h"

using wsl::windows::common::HandleConsoleProgressBar;

HandleConsoleProgressBar::HandleConsoleProgressBar(HANDLE handle, std::wstring&& message, Format format)
{
    // If this file isn't a disk file, we can't show actual progress. Just show an indicator in that case
    LARGE_INTEGER fileSize{};
    if (GetFileType(handle) != FILE_TYPE_DISK || FAILED(GetFileSizeEx(handle, &fileSize)))
    {
        m_progressBar.emplace<ConsoleProgressIndicator>(std::move(message));
    }
    else if (format == FileSize)
    {
        auto& progressBar = m_progressBar.emplace<ConsoleProgressIndicator>(std::move(message), false);

        m_thread = std::thread([handle, &progressBar, this]() {
            UpdateFileSize(handle, progressBar);
            progressBar.End();
        });
    }
    else
    {
        WI_ASSERT(format == FilePointer);

        auto& progressBar = m_progressBar.emplace<ConsoleProgressBar>();

        m_thread = std::thread([handle, fileSize, &progressBar, this]() {
            UpdateProgress(handle, fileSize, progressBar);
            progressBar.Clear();
        });
    }
}

void HandleConsoleProgressBar::UpdateProgress(HANDLE handle, LARGE_INTEGER fileSize, ConsoleProgressBar& progressBar) const
try
{
    while (!m_event.is_signaled())
    {
        LARGE_INTEGER position{};
        THROW_IF_WIN32_BOOL_FALSE(SetFilePointerEx(handle, LARGE_INTEGER{}, &position, FILE_CURRENT));

        constexpr auto progressResolution = 1000;
        const auto progressRatio = (position.QuadPart / (double)fileSize.QuadPart) * progressResolution;

        progressBar.Print(static_cast<UINT>(progressRatio), progressResolution);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
CATCH_LOG();

void HandleConsoleProgressBar::UpdateFileSize(HANDLE handle, ConsoleProgressIndicator& progressBar) const
{
    try
    {
        while (!m_event.is_signaled())
        {
            LARGE_INTEGER size{};
            THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(handle, &size));

            progressBar.UpdateProgress(std::format(L" ({} MB)", size.QuadPart / _1MB));

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    CATCH_LOG();
}

HandleConsoleProgressBar::~HandleConsoleProgressBar()
{
    if (m_thread.joinable())
    {
        m_event.SetEvent();
        m_thread.join();
    }
}