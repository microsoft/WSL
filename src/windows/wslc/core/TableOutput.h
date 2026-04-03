/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableOutput.h

Abstract:

    Header file for outputting data in a table format.

--*/
#pragma once

#include <algorithm>
#include <array>
#include <cwchar>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <wslutil.h>

namespace wsl::windows::wslc {

namespace detail {
    // This function outputs a table line.
    inline void PrintTableLine(const std::wstring& line, FILE* stream)
    {
        ::wsl::windows::common::wslutil::PrintMessage(line, stream);
    }
} // namespace detail

// Helper function to get display width of a string
// For now, uses simple length (can be enhanced with proper Unicode width calculation)
inline size_t GetStringColumnWidth(const wchar_t* str)
{
    if (!str)
    {
        return 0;
    }
    return wcslen(str);
}

// Helper function to trim string to a specific column width
inline std::wstring TrimStringToColumnWidth(const wchar_t* str, size_t maxWidth, size_t& actualWidth)
{
    if (!str)
    {
        actualWidth = 0;
        return L"";
    }

    size_t len = wcslen(str);
    if (len <= maxWidth)
    {
        actualWidth = len;
        return std::wstring(str);
    }

    actualWidth = maxWidth;
    return std::wstring(str, maxWidth);
}

// Column width configuration options
struct ColumnWidthConfig
{
    static constexpr size_t NoLimit = 0;

    size_t MinWidth = NoLimit;   // Minimum column width (NoLimit = use header width)
    size_t MaxWidth = NoLimit;   // Maximum column width (NoLimit = unlimited)
    bool PreferredShrink = true; // Should this column shrink first when space is limited?
};

// Column definition with name and configuration
struct ColumnDefinition
{
    std::wstring Name;
    ColumnWidthConfig Config;
};

// Enables output data in a table format.
// TODO: Improve for use with sparse data.
template <size_t FieldCount>
struct TableOutput
{
    static_assert(FieldCount > 0, "TableOutput requires at least one column");

    using header_t = std::array<std::wstring, FieldCount>;
    using line_t = std::array<std::wstring, FieldCount>;
    using column_config_t = std::array<ColumnWidthConfig, FieldCount>;
    using column_def_t = std::array<ColumnDefinition, FieldCount>;
    using OutputFn = std::function<void(const std::wstring&)>;

    static constexpr size_t DefaultColumnPadding = 3; // Docker-like spacing between columns

    // Constructor with default behavior (no column limits)
    TableOutput(header_t&& header, size_t sizingBuffer = 50, size_t columnPadding = DefaultColumnPadding) :
        m_sizingBuffer(sizingBuffer), m_limitColumnWidths(false), m_columnPadding(columnPadding), m_outputFn(DefaultOutputFn())
    {
        InitializeColumns(std::move(header));
    }

    // Constructor with column width configuration (legacy)
    TableOutput(header_t&& header, column_config_t&& config, size_t sizingBuffer = 50, size_t columnPadding = DefaultColumnPadding) :
        m_sizingBuffer(sizingBuffer),
        m_limitColumnWidths(true),
        m_columnPadding(columnPadding),
        m_columnConfigs(std::move(config)),
        m_outputFn(DefaultOutputFn())
    {
        InitializeColumns(std::move(header));
    }

    // Constructor with column definitions (name + config together)
    TableOutput(column_def_t&& columns, size_t sizingBuffer = 50, size_t columnPadding = DefaultColumnPadding) :
        m_sizingBuffer(sizingBuffer), m_limitColumnWidths(true), m_columnPadding(columnPadding), m_outputFn(DefaultOutputFn())
    {
        header_t headers;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            headers[i] = std::move(columns[i].Name);
            m_columnConfigs[i] = columns[i].Config;
        }
        InitializeColumns(std::move(headers));
    }

    // Enable/disable column width limiting
    void SetColumnWidthLimiting(bool enable)
    {
        m_limitColumnWidths = enable;
    }

    // Set configuration for a specific column
    void SetColumnConfig(size_t columnIndex, const ColumnWidthConfig& config)
    {
        if (columnIndex < FieldCount)
        {
            m_columnConfigs[columnIndex] = config;
        }
    }

    // Set whether to always show header even when there are no rows
    void SetAlwaysShowHeader(bool alwaysShow)
    {
        m_alwaysShowHeader = alwaysShow;
    }

    // Override the output function (e.g. redirect to a stringstream in tests).
    void SetOutputFunction(OutputFn fn)
    {
        m_outputFn = std::move(fn);
    }

    // Override the console width used for column shrinking (useful in tests).
    // Pass 0 to restore the default behaviour (query the real console).
    void SetConsoleWidthOverride(size_t width)
    {
        m_consoleWidthOverride = width;
    }

    void OutputLine(line_t&& line)
    {
        m_empty = false;

        // When width limiting is disabled, buffer all rows to ensure accurate column sizing
        // and prevent truncation (e.g., for --no-trunc flag)
        if (!m_limitColumnWidths || m_buffer.size() < m_sizingBuffer)
        {
            m_buffer.emplace_back(std::move(line));
        }
        else
        {
            EvaluateAndFlushBuffer();
            OutputLineToStream(line);
        }
    }

    void Complete()
    {
        if (!m_empty)
        {
            EvaluateAndFlushBuffer();
        }
        else if (m_alwaysShowHeader)
        {
            OutputHeaderOnly();
        }
    }

    bool IsEmpty()
    {
        return m_empty;
    }

private:
    // A column in the table.
    struct Column
    {
        std::wstring Name;
        size_t MinLength = 0;
        size_t MaxLength = 0;
        size_t ConfiguredMaxLength = 0; // Max length from configuration
        bool SpaceAfter = true;
    };

    std::array<Column, FieldCount> m_columns;
    column_config_t m_columnConfigs;
    size_t m_sizingBuffer;
    size_t m_columnPadding;
    std::vector<line_t> m_buffer;
    bool m_bufferEvaluated = false;
    bool m_empty = true;
    bool m_limitColumnWidths = false;
    bool m_alwaysShowHeader = true;
    std::wstringstream m_stream;
    OutputFn m_outputFn;
    size_t m_consoleWidthOverride = 0;

    static OutputFn DefaultOutputFn()
    {
        return [](const std::wstring& line) { detail::PrintTableLine(line, stdout); };
    }

    void InitializeColumns(header_t&& header)
    {
        for (size_t i = 0; i < FieldCount; ++i)
        {
            m_columns[i].Name = std::move(header[i]);
            m_columns[i].MinLength = GetStringColumnWidth(m_columns[i].Name.c_str());
            m_columns[i].MaxLength = 0;

            // Apply configured max width if limiting is enabled
            if (m_limitColumnWidths && m_columnConfigs[i].MaxWidth != ColumnWidthConfig::NoLimit)
            {
                m_columns[i].ConfiguredMaxLength = m_columnConfigs[i].MaxWidth;
            }

            // Apply configured min width
            if (m_columnConfigs[i].MinWidth != ColumnWidthConfig::NoLimit)
            {
                m_columns[i].MinLength = std::max(m_columns[i].MinLength, m_columnConfigs[i].MinWidth);
            }
        }
    }

    size_t GetConsoleWidth()
    {
        if (m_consoleWidthOverride > 0)
        {
            return m_consoleWidthOverride;
        }

        CONSOLE_SCREEN_BUFFER_INFO consoleInfo{};
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

        if (GetConsoleScreenBufferInfo(hConsole, &consoleInfo))
        {
            return static_cast<size_t>(consoleInfo.srWindow.Right - consoleInfo.srWindow.Left + 1);
        }

        // Default to 80 columns if console info is unavailable
        return 80;
    }

    void OutputHeaderOnly()
    {
        // Set MaxLength to MinLength for all columns (header width only)
        for (size_t i = 0; i < FieldCount; ++i)
        {
            m_columns[i].MaxLength = m_columns[i].MinLength;
        }

        // Set spacing configuration
        m_columns[FieldCount - 1].SpaceAfter = false;

        // Output the header
        line_t headerLine;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            headerLine[i] = m_columns[i].Name.c_str();
        }

        OutputLineToStream(headerLine);
        m_bufferEvaluated = true;
    }

    void EvaluateAndFlushBuffer()
    {
        if (m_bufferEvaluated)
        {
            return;
        }

        // Determine the maximum length for all columns
        for (const auto& line : m_buffer)
        {
            for (size_t i = 0; i < FieldCount; ++i)
            {
                size_t columnWidth = GetStringColumnWidth(line[i].c_str());

                // Apply configured max width if limiting is enabled
                if (m_limitColumnWidths && m_columns[i].ConfiguredMaxLength != ColumnWidthConfig::NoLimit)
                {
                    columnWidth = std::min(columnWidth, m_columns[i].ConfiguredMaxLength);
                }

                m_columns[i].MaxLength = std::max(m_columns[i].MaxLength, columnWidth);
            }
        }

        // If there are actually columns with data, then also bring in the minimum size
        for (size_t i = 0; i < FieldCount; ++i)
        {
            if (m_columns[i].MaxLength)
            {
                m_columns[i].MaxLength = std::max(m_columns[i].MaxLength, m_columns[i].MinLength);
            }
        }

        // Only output the extra space if:
        // 1. Not the last field
        m_columns[FieldCount - 1].SpaceAfter = false;

        // 2. Not empty (taken care of by not doing anything if empty)
        // 3. There are non-empty fields after
        for (size_t i = FieldCount - 1; i > 0; --i)
        {
            if (m_columns[i].MaxLength)
            {
                break;
            }
            else
            {
                m_columns[i - 1].SpaceAfter = false;
            }
        }

        // Determine the total width required to not truncate any columns
        size_t totalRequired = 0;

        for (size_t i = 0; i < FieldCount; ++i)
        {
            totalRequired += m_columns[i].MaxLength + (m_columns[i].SpaceAfter ? m_columnPadding : 0);
        }

        // Only apply console width constraints if m_limitColumnWidths is true
        if (m_limitColumnWidths)
        {
            size_t consoleWidth = GetConsoleWidth();

            // If the total space would be too big, shrink them.
            // We don't want to use the last column, lest we auto-wrap
            if (totalRequired >= consoleWidth)
            {
                size_t extra = (totalRequired - consoleWidth) + 1;

                while (extra > 0)
                {
                    // Find the largest shrinkable column
                    size_t targetIndex = 0;
                    size_t targetVal = 0;

                    for (size_t j = 0; j < FieldCount; ++j)
                    {
                        // Skip columns at or below minimum
                        if (m_columns[j].MaxLength <= m_columns[j].MinLength)
                        {
                            continue;
                        }

                        // Prefer columns marked as preferredShrink
                        bool isPreferredShrink = m_columnConfigs[j].PreferredShrink;
                        bool currentIsPreferred = m_columnConfigs[targetIndex].PreferredShrink;

                        if (isPreferredShrink && !currentIsPreferred)
                        {
                            targetIndex = j;
                            targetVal = m_columns[j].MaxLength;
                        }
                        else if (isPreferredShrink == currentIsPreferred && m_columns[j].MaxLength > targetVal)
                        {
                            targetIndex = j;
                            targetVal = m_columns[j].MaxLength;
                        }
                    }

                    // If no shrinkable column found, break
                    if (targetVal == 0)
                    {
                        break;
                    }

                    m_columns[targetIndex].MaxLength -= 1;
                    extra -= 1;
                }

                totalRequired = std::min(totalRequired, consoleWidth - 1);
            }
        }

        line_t headerLine;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            headerLine[i] = m_columns[i].Name.c_str();
        }

        OutputLineToStream(headerLine);

        for (const auto& line : m_buffer)
        {
            OutputLineToStream(line);
        }

        m_bufferEvaluated = true;
    }

    void OutputLineToStream(const line_t& line)
    {
        for (size_t i = 0; i < FieldCount; ++i)
        {
            const auto& col = m_columns[i];

            if (col.MaxLength)
            {
                size_t valueLength = GetStringColumnWidth(line[i].c_str());

                if (valueLength > col.MaxLength)
                {
                    size_t actualWidth;
                    m_stream << TrimStringToColumnWidth(line[i].c_str(), col.MaxLength - 1, actualWidth) << L"\u2026"; // Unicode ellipsis character

                    // Some characters take 2 unit space, the trimmed string length might be 1 less than the expected length.
                    if (actualWidth != col.MaxLength - 1)
                    {
                        m_stream << L' ';
                    }

                    if (col.SpaceAfter)
                    {
                        m_stream << std::wstring(m_columnPadding, L' ');
                    }
                }
                else
                {
                    m_stream << line[i];

                    if (col.SpaceAfter)
                    {
                        m_stream << std::wstring(col.MaxLength - valueLength + m_columnPadding, L' ');
                    }
                }
            }
        }

        const std::wstring rendered = m_stream.str();
        m_stream.str(L"");
        m_stream.clear();

        m_outputFn(rendered);
    }
};

} // namespace wsl::windows::wslc
