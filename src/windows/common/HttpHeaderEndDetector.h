/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HttpHeaderEndDetector.h

Abstract:

    This file contains a small state machine that detects the end-of-header
    marker ("\r\n\r\n") in an HTTP message, one byte at a time.

--*/

#pragma once

namespace wsl::windows::common {

// Detects the end-of-header marker ("\r\n\r\n") in an HTTP message
class HttpHeaderEndDetector
{
public:
    // Returns true once the full "\r\n\r\n" terminator has been consumed.
    bool Consume(char byte) noexcept
    {
        switch (m_state)
        {
        case State::Start:
            m_state = (byte == '\r') ? State::Cr : State::Start;
            break;
        case State::Cr:
            m_state = (byte == '\n') ? State::CrLf : ((byte == '\r') ? State::Cr : State::Start);
            break;
        case State::CrLf:
            m_state = (byte == '\r') ? State::CrLfCr : State::Start;
            break;
        case State::CrLfCr:
            m_state = (byte == '\n') ? State::Done : ((byte == '\r') ? State::Cr : State::Start);
            break;
        case State::Done:
            break;
        }

        return m_state == State::Done;
    }

    bool IsDone() const noexcept
    {
        return m_state == State::Done;
    }

private:
    enum class State
    {
        Start,
        Cr,
        CrLf,
        CrLfCr,
        Done
    };

    State m_state = State::Start;
};

} // namespace wsl::windows::common
