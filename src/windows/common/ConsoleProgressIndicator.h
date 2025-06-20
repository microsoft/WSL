/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleProgressIndicator.h

Abstract:

    This file contains console function declarations.

--*/

#pragma once

#include <thread>
#include "wrl.h"

namespace wsl::windows::common {
class ConsoleProgressIndicator
{
private:
    std::thread m_thread;
    wil::unique_event m_event;
    std::wstring m_waitMessage;
    std::wstring m_progressMessage;
    bool m_interactive{};

    void IndicateProgress() const;

public:
    ConsoleProgressIndicator(std::wstring&& inputMessage, bool animatedDots = false);
    ~ConsoleProgressIndicator();

    void End();

    void UpdateProgress(std::wstring&& progress);
};
} // namespace wsl::windows::common
