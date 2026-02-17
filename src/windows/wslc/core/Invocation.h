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

namespace wsl::windows::wslc {
struct Invocation
{
    Invocation(std::vector<std::wstring>&& args) : m_args(std::move(args))
    {
        m_rawCommandLine = GetCommandLineW();
    }

    // Constructor for unit testing.
    Invocation(std::vector<std::wstring>&& args, const wchar_t* rawCommandLine) : m_args(std::move(args))
    {
        if (rawCommandLine != nullptr)
        {
            m_rawCommandLine = rawCommandLine;
        }
    }

    struct iterator
    {
        iterator(size_t arg, std::vector<std::wstring>& args) : m_arg(arg), m_args(args)
        {
        }

        iterator(const iterator&) = default;
        iterator& operator=(const iterator&) = default;

        iterator operator++()
        {
            return {++m_arg, m_args};
        }
        iterator operator++(int)
        {
            return {m_arg++, m_args};
        }
        iterator operator--()
        {
            return {--m_arg, m_args};
        }
        iterator operator--(int)
        {
            return {m_arg--, m_args};
        }

        bool operator==(const iterator& other) const
        {
            return m_arg == other.m_arg;
        }
        bool operator!=(const iterator& other) const
        {
            return m_arg != other.m_arg;
        }

        const std::wstring& operator*() const
        {
            return m_args[m_arg];
        }
        const std::wstring* operator->() const
        {
            return &(m_args[m_arg]);
        }

        size_t index() const
        {
            return m_arg;
        }

    private:
        size_t m_arg;
        std::vector<std::wstring>& m_args;
    };

    size_t size() const
    {
        return m_args.size();
    }
    iterator begin()
    {
        return {m_currentFirstArg, m_args};
    }
    iterator end()
    {
        return {m_args.size(), m_args};
    }
    void consume(const iterator& i)
    {
        m_currentFirstArg = i.index() + 1;
    }

    std::wstring_view GetRemainingRawCommandLineFromIndex(size_t argvIndex) const noexcept
    {
        // Lazy initialized only when needed, and any allocation errors are swallowed.
        if (!m_mappingInitialized)
        {
            try
            {
                MapArgvToRawCommandLine();
                m_mappingInitialized = true;
            }
            catch (...)
            {
                // TODO: Log this situation.
                return {};
            }
        }

        if (m_rawCommandLine.empty() || argvIndex >= m_argvPositions.size() || m_argvPositions[argvIndex] == std::wstring::npos)
        {
            return {};
        }

        size_t startPos = m_argvPositions[argvIndex];
        if (startPos >= m_rawCommandLine.length())
        {
            return {};
        }

        return std::wstring_view(m_rawCommandLine).substr(startPos);
    }

private:
    // This creates a map of argv index to the position in the raw command line.
    // where that argument begins, allowing return of raw command line at any index.
    void MapArgvToRawCommandLine() const noexcept
    {
        m_argvPositions.resize(m_args.size(), std::wstring::npos);

        if (m_rawCommandLine.empty() || m_args.empty())
        {
            return;
        }

        size_t pos = SkipExecutablePath();
        for (size_t argIndex = 0; argIndex < m_args.size(); ++argIndex)
        {
            while (pos < m_rawCommandLine.length() && (m_rawCommandLine[pos] == L' ' || m_rawCommandLine[pos] == L'\t'))
            {
                pos++;
            }

            if (pos >= m_rawCommandLine.length())
            {
                break;
            }

            m_argvPositions[argIndex] = pos;
            pos = AdvancePastArgument(pos);
        }
    }

    size_t SkipExecutablePath() const noexcept
    {
        if (m_rawCommandLine.empty())
        {
            return 0;
        }

        size_t pos = 0;

        // Executable path may be quoted with "...". Anything in between is taken as-is.
        // https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments?view=msvc-170
        if (m_rawCommandLine[0] == L'"')
        {
            // Find the closing quote
            size_t closingQuote = m_rawCommandLine.find(L'"', 1);
            if (closingQuote != std::wstring::npos)
            {
                pos = closingQuote + 1;
            }
            else
            {
                // No closing quote found - fall back to space.
                pos = m_rawCommandLine.find(L' ');
                if (pos == std::wstring::npos)
                {
                    return m_rawCommandLine.length();
                }
            }
        }
        else
        {
            pos = m_rawCommandLine.find(L' ');
            if (pos == std::wstring::npos)
            {
                return m_rawCommandLine.length();
            }
        }

        return pos;
    }

    size_t AdvancePastArgument(size_t startPos) const noexcept
    {
        if (startPos >= m_rawCommandLine.length())
        {
            return startPos;
        }

        bool inQuotes = false;
        size_t pos = startPos;

        // Parse using Windows command line rules for CommandLineToArgvW
        // - 2n backslashes + quote -> n backslashes, toggle quote mode
        // - 2n+1 backslashes + quote -> n backslashes, literal quote
        // - backslashes not before quote -> literal backslashes
        // - "" -> literal quote AND toggle quote mode (undocumented rule)
        // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw
        while (pos < m_rawCommandLine.length())
        {
            wchar_t ch = m_rawCommandLine[pos];

            if (ch == L'\\')
            {
                size_t backslashCount = 0;
                while (pos < m_rawCommandLine.length() && m_rawCommandLine[pos] == L'\\')
                {
                    backslashCount++;
                    pos++;
                }

                if (pos < m_rawCommandLine.length() && m_rawCommandLine[pos] == L'"')
                {
                    if (backslashCount % 2 == 0)
                    {
                        inQuotes = !inQuotes;
                    }
                    pos++;
                }
            }
            else if (ch == L'"')
            {
                // Check for "" (two consecutive quotes)
                // This is a special known deviation of CommandLineToArgvW where "" produces a literal quote AND exits quoted
                // mode. Commentary on the undocumented rule: https://stackoverflow.com/a/3476890
                if (inQuotes && pos + 1 < m_rawCommandLine.length() && m_rawCommandLine[pos + 1] == L'"')
                {
                    // Two quotes: produces literal quote AND exits quoted mode (CommandLineToArgvW quirk!)
                    pos += 2;         // Skip both quotes
                    inQuotes = false; // Exit quoted mode
                }
                else
                {
                    // Single quote: toggle quote mode
                    inQuotes = !inQuotes;
                    pos++;
                }
            }
            else if ((ch == L' ' || ch == L'\t') && !inQuotes)
            {
                break;
            }
            else
            {
                pos++;
            }
        }

        return pos;
    }

    std::vector<std::wstring> m_args;
    size_t m_currentFirstArg = 0;
    std::wstring m_rawCommandLine;
    mutable std::vector<size_t> m_argvPositions; // Lazy initialized
    mutable bool m_mappingInitialized = false;
};
} // namespace wsl::windows::wslc
