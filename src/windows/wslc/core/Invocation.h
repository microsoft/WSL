/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Invocation.h

Abstract:

    Header file for walking through and processing a command line invocation.

--*/
#pragma once
#include <string>
#include <vector>

namespace wsl::windows::wslc {
struct InvocationCursor
{
    InvocationCursor(std::vector<std::wstring>&& arguments) : m_arguments(std::move(arguments))
    {
    }

    struct iterator
    {
        iterator(size_t argument, const std::vector<std::wstring>& arguments) : m_argument(argument), m_arguments(arguments)
        {
        }

        iterator(const iterator&) = default;
        iterator& operator=(const iterator&) = default;

        iterator& operator++()
        {
            ++m_argument;
            return *this;
        }
        iterator operator++(int)
        {
            auto previous = *this;
            ++(*this);
            return previous;
        }
        iterator& operator--()
        {
            --m_argument;
            return *this;
        }
        iterator operator--(int)
        {
            auto previous = *this;
            --(*this);
            return previous;
        }

        bool operator==(const iterator& other) const
        {
            return m_argument == other.m_argument;
        }
        bool operator!=(const iterator& other) const
        {
            return m_argument != other.m_argument;
        }

        const std::wstring& operator*() const
        {
            return m_arguments[m_argument];
        }
        const std::wstring* operator->() const
        {
            return &m_arguments[m_argument];
        }

        size_t index() const
        {
            return m_argument;
        }

    private:
        size_t m_argument;
        const std::vector<std::wstring>& m_arguments;
    };

    size_t size() const
    {
        return m_arguments.size();
    }
    const std::vector<std::wstring>& OriginalArguments() const noexcept
    {
        return m_arguments;
    }
    size_t Position() const noexcept
    {
        return m_position;
    }
    iterator begin() const
    {
        return {m_position, m_arguments};
    }
    iterator end() const
    {
        return {m_arguments.size(), m_arguments};
    }
    void AdvancePast(const iterator& position)
    {
        m_position = position.index() + 1;
    }
    void SetPosition(const iterator& position)
    {
        m_position = position.index();
    }

private:
    std::vector<std::wstring> m_arguments;
    size_t m_position = 0;
};
} // namespace wsl::windows::wslc
