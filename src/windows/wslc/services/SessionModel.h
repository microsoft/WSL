/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionModel.h

Abstract:

    This file contains the SessionModel definition

--*/
#pragma once

#include <wslc.h>

namespace wsl::windows::wslc::models {

struct Session
{
    explicit Session(wil::com_ptr<IWSLCSession> session) : m_session(std::move(session))
    {
    }

    Session(wil::com_ptr<IWSLCSession> session, wil::unique_handle warningsPipeRead, wil::unique_handle warningsPipeWrite) :
        m_session(std::move(session)), m_warningsPipeWrite(std::move(warningsPipeWrite))
    {
        // Spawn a background thread that reads warnings from the pipe and prints them to stderr in real-time.
        m_warningsThread = std::thread([read = std::move(warningsPipeRead)]() {
            try
            {
                wchar_t buffer[1024] = {0};
                DWORD bytesRead{};
                while (ReadFile(read.get(), buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, nullptr) && bytesRead > 0)
                {
                    const auto endIndex = bytesRead / sizeof(wchar_t);
                    buffer[endIndex] = UNICODE_NULL;
                    fwprintf(stderr, L"%ls", buffer);
                }
            }
            CATCH_LOG();
        });
    }

    ~Session()
    {
        // Close the write end of the pipe so the reader thread exits.
        m_warningsPipeWrite.reset();
        if (m_warningsThread.joinable())
        {
            m_warningsThread.join();
        }
    }

    NON_COPYABLE(Session);
    Session(Session&&) = default;
    Session& operator=(Session&&) = default;

    IWSLCSession* Get() const noexcept
    {
        return m_session.get();
    }

    HANDLE WarningsPipeWrite() const noexcept
    {
        return m_warningsPipeWrite.get();
    }

private:
    wil::com_ptr<IWSLCSession> m_session;
    wil::unique_handle m_warningsPipeWrite;
    std::thread m_warningsThread;
};

} // namespace wsl::windows::wslc::models
