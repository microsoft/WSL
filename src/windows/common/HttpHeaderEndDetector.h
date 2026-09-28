// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <algorithm>
#include <deque>
#include <string_view>

namespace wsl::windows::common {

// Detects the end-of-header marker ("\r\n\r\n") in an HTTP message
class HttpHeaderEndDetector
{
public:
    // Returns true once the full "\r\n\r\n" terminator has been consumed.
    bool Consume(char byte)
    {
        if (m_done)
        {
            return true;
        }

        m_last4Bytes.push_back(byte);
        if (m_last4Bytes.size() > 4)
        {
            m_last4Bytes.pop_front();
        }

        static constexpr std::string_view c_terminator = "\r\n\r\n";
        m_done = std::ranges::equal(m_last4Bytes, c_terminator);
        return m_done;
    }

    bool IsDone() const noexcept
    {
        return m_done;
    }

private:
    std::deque<char> m_last4Bytes;
    bool m_done = false;
};

} // namespace wsl::windows::common
