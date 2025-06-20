/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HandleConsoleProgressBar.h

Abstract:

    This file contains the definition of the HandleConsoleProgressBar class.

--*/

#pragma once

#include "ConsoleProgressBar.h"
#include "ConsoleProgressIndicator.h"

namespace wsl::windows::common {
class HandleConsoleProgressBar
{
public:
    enum Format
    {
        FilePointer,
        FileSize
    };

    HandleConsoleProgressBar(HANDLE handle, std::wstring&& message, Format format = FilePointer);
    HandleConsoleProgressBar(const HandleConsoleProgressBar&) = delete;
    HandleConsoleProgressBar(HandleConsoleProgressBar&&) = delete;

    ~HandleConsoleProgressBar();

    HandleConsoleProgressBar& operator=(const HandleConsoleProgressBar&) = delete;
    HandleConsoleProgressBar& operator=(HandleConsoleProgressBar&&) = delete;

private:
    void UpdateProgress(HANDLE handle, LARGE_INTEGER totalBytes, ConsoleProgressBar& progressBar) const;
    void UpdateFileSize(HANDLE handle, ConsoleProgressIndicator& progressBar) const;

    wil::unique_event m_event{wil::EventOptions::ManualReset};
    std::thread m_thread;
    std::variant<ConsoleProgressBar, ConsoleProgressIndicator> m_progressBar;
};

} // namespace wsl::windows::common