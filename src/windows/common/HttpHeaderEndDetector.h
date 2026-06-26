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
    bool Consume(char Byte) noexcept
    {
        switch (m_state)
        {
        case State::Start:
            m_state = (Byte == '\r') ? State::Cr : State::Start;
            break;
        case State::Cr:
            m_state = (Byte == '\n') ? State::CrLf : ((Byte == '\r') ? State::Cr : State::Start);
            break;
        case State::CrLf:
            m_state = (Byte == '\r') ? State::CrLfCr : State::Start;
            break;
        case State::CrLfCr:
            m_state = (Byte == '\n') ? State::Done : ((Byte == '\r') ? State::Cr : State::Start);
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
