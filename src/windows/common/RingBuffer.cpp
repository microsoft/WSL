/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RingBuffer.cpp

Abstract:

    This file contains definitions for the RingBuffer class.

--*/

#include "precomp.h"
#include "RingBuffer.h"

RingBuffer::RingBuffer(size_t size) : m_maxSize(size), m_offset(0)
{
    m_buffer.reserve(size);
}

void RingBuffer::Insert(std::string_view data)
{
    auto lock = m_lock.lock_exclusive();
    auto remainingData = gsl::make_span(data.data(), data.size());
    if (remainingData.size() > m_maxSize)
    {
        remainingData = remainingData.subspan(remainingData.size() - m_maxSize);
    }

    const auto bytesAtEnd = std::min(m_maxSize - m_offset, remainingData.size());
    if (m_offset + bytesAtEnd > m_buffer.size())
    {
        m_buffer.resize(m_offset + bytesAtEnd);
        WI_ASSERT(m_buffer.size() <= m_maxSize);
    }

    const auto allBuffer = gsl::make_span(m_buffer);
    const auto beginCopyBuffer = allBuffer.subspan(m_offset, bytesAtEnd);
    copy(remainingData.subspan(0, bytesAtEnd), beginCopyBuffer);
    remainingData = remainingData.subspan(bytesAtEnd);
    if (!remainingData.empty())
    {
        copy(remainingData, allBuffer);
        m_offset = remainingData.size();
    }
    else
    {
        m_offset += bytesAtEnd;
    }
}

std::vector<std::string> RingBuffer::GetLastDelimitedStrings(char Delimiter, size_t Count) const
{
    auto lock = m_lock.lock_shared();
    auto [begin, end] = Contents();
    std::vector<std::string> results;
    std::optional<size_t> endIndex;
    for (size_t i = end.size(); i > 0; i--)
    {
        if (results.size() == Count)
        {
            break;
        }

        if (Delimiter == end[i - 1])
        {
            if (endIndex.has_value())
            {
                results.emplace(results.begin(), &end[i], endIndex.value() - i);
                endIndex.reset();
            }
            else
            {
                endIndex = i - 1;
            }
        }
    }

    if (results.size() == Count)
    {
        return results;
    }

    std::string partial;
    if (endIndex.has_value())
    {
        partial = std::string{&end[0], endIndex.value()};
        endIndex.reset();
    }

    for (size_t i = begin.size(); i > 0; i--)
    {
        if (results.size() == Count)
        {
            break;
        }

        if (Delimiter == begin[i - 1])
        {
            if (!partial.empty())
            {
                // The debug CRT will fastfail if begin[size] is accessed
                // But in this case it's not a problem because begin.size() - i would be == 0
                std::string partial_begin{&begin.data()[i], begin.size() - i};
                results.emplace(results.begin(), partial_begin + partial);
                partial.clear();
            }
            else if (endIndex.has_value())
            {
                results.emplace(results.begin(), &begin.data()[i], endIndex.value() - i);
                endIndex.reset();
            }
            else
            {
                endIndex = i - 1;
            }
        }
    }

    if (results.size() < Count)
    {
        // May have lost some data, or this could be the very first line logged.
        if (!partial.empty())
        {
            results.emplace(results.begin(), partial);
        }
        else if (endIndex.has_value())
        {
            results.emplace(results.begin(), &begin[0], endIndex.value());
        }
    }

    return results;
}

std::string RingBuffer::Get() const
{
    auto lock = m_lock.lock_shared();
    auto [begin, end] = Contents();
    std::string data;
    data.reserve(begin.size() + end.size());
    data.append(begin.data(), begin.size());
    data.append(end.data(), end.size());
    return data;
}

std::pair<std::string_view, std::string_view> RingBuffer::Contents() const
{
    std::string_view beginView(m_buffer.data() + m_offset, m_buffer.size() - m_offset);
    std::string_view endView(m_buffer.data(), m_offset);
    return {beginView, endView};
}