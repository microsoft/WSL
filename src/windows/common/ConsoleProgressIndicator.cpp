/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleProgressIndicator.cpp

Abstract:

    This file contains the ConsoleProgressIndicator implementation

--*/

#include "precomp.h"
#include "ConsoleProgressIndicator.h"
#include "wslutil.h"

wsl::windows::common::ConsoleProgressIndicator::ConsoleProgressIndicator(std::wstring&& inputMessage, bool animatedDots)
{
    m_waitMessage = std::move(inputMessage);

    // Start the thread to update progress bar only if it's a tty
    if (_isatty(_fileno(stderr)))
    {
        m_interactive = true;

        if (animatedDots)
        {
            m_event.create(wil::EventOptions::ManualReset);
            m_thread = std::thread([&] { IndicateProgress(); });
        }
        else
        {
            fwprintf(stderr, L"%ls", m_waitMessage.c_str());
        }
    }
}

wsl::windows::common::ConsoleProgressIndicator::~ConsoleProgressIndicator()
{
    End();
}

void wsl::windows::common::ConsoleProgressIndicator::UpdateProgress(std::wstring&& Progress)
{
    if (!m_interactive)
    {
        return;
    }

    for (auto i = 0; i < m_progressMessage.size(); i++)
    {
        fwprintf(stderr, L"\b \b");
    }

    fwprintf(stderr, L"%ls", Progress.c_str());
    m_progressMessage = std::move(Progress);
}

void wsl::windows::common::ConsoleProgressIndicator::IndicateProgress() const
{
    fwprintf(stderr, L"%ls", m_waitMessage.c_str());

    // Print status until the exit event is signaled.
    int currentDots = 0;
    constexpr int c_maxDots = 3;
    while (!m_event.wait(500))
    {
        if (currentDots < c_maxDots)
        {
            fwprintf(stderr, L".");
            currentDots++;
        }
        else
        {
            for (int i = 0; i < c_maxDots; i++)
            {
                fwprintf(stderr, L"\b \b");
            }

            currentDots = 0;
        }
    }

    // Clear any dots that remain.
    for (int i = 0; i < currentDots; i++)
    {
        fwprintf(stderr, L"\b \b");
    }
}

void wsl::windows::common::ConsoleProgressIndicator::End()
{
    // If the thread started, trigger exit event and join thread.
    if (m_thread.joinable())
    {
        m_event.SetEvent();
        m_thread.join();
    }

    if (m_interactive)
    {
        fwprintf(stderr, L"\n");
    }
}
