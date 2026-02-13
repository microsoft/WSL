/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Invocation.h

Abstract:

    Header file for walking through and processing a command line invocation.

--*/
#pragma once
#include <map>
#include <string>
#include <vector>

namespace wsl::windows::wslc
{
    // Contains the raw command line arguments and functionality to iterate and consume them.
    struct Invocation
    {
        Invocation(std::vector<std::wstring>&& args) : m_args(std::move(args)) {}

        struct iterator
        {
            iterator(size_t arg, std::vector<std::wstring>& args) : m_arg(arg), m_args(args) {}

            iterator(const iterator&) = default;
            iterator& operator=(const iterator&) = default;

            iterator operator++() { return { ++m_arg, m_args }; }
            iterator operator++(int) { return { m_arg++, m_args }; }
            iterator operator--() { return { --m_arg, m_args }; }
            iterator operator--(int) { return { m_arg--, m_args }; }

            bool operator==(const iterator& other) const { return m_arg == other.m_arg; }
            bool operator!=(const iterator& other) const { return m_arg != other.m_arg; }

            const std::wstring& operator*() const { return m_args[m_arg]; }
            const std::wstring* operator->() const { return &(m_args[m_arg]); }

            size_t index() const { return m_arg; }

        private:
            size_t m_arg;
            std::vector<std::wstring>& m_args;
        };

        size_t size() const { return m_args.size(); }
        iterator begin() { return { m_currentFirstArg, m_args }; }
        iterator end() { return { m_args.size(), m_args }; }
        void consume(const iterator& i) { m_currentFirstArg = i.index() + 1; }

    private:
        std::vector<std::wstring> m_args;
        size_t m_currentFirstArg = 0;
    };
}
