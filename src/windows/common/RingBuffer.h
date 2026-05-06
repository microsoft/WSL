/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RingBuffer.h

Abstract:

    This file contains declarations for the RingBuffer class.

--*/

#pragma once

class RingBuffer
{
public:
    RingBuffer() = delete;
    RingBuffer(size_t size);

    void Insert(std::string_view data);
    std::vector<std::string> GetLastDelimitedStrings(char Delimiter, size_t Count) const;
    std::string Get() const;

private:
    std::pair<std::string_view, std::string_view> Contents() const;

    mutable wil::srwlock m_lock;
    std::vector<char> m_buffer;
    size_t m_maxSize;
    size_t m_offset;
};