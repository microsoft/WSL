// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

/*
#include "ExecutionReporter.h"

#include <array>
#include <ostream>
#include <string>
#include <vector>

namespace wsl::windows::wslc::execution
{
    // Enables output data in a table format.
    // TODO: Improve for use with sparse data.
    template <size_t FieldCount>
    struct TableOutput
    {
        using header_t = std::array<std::wstring, FieldCount>;
        using line_t = std::array<std::wstring, FieldCount>;

        TableOutput(Reporter& reporter, header_t&& header, size_t sizingBuffer = 50) :
            m_reporter(reporter), m_sizingBuffer(sizingBuffer)
        {
            for (size_t i = 0; i < FieldCount; ++i)
            {
                m_columns[i].Name = std::move(header[i]);
                m_columns[i].MinLength = Utility::UTF8ColumnWidth(m_columns[i].Name.get());
                m_columns[i].MaxLength = 0;
            }
        }

        void OutputLine(line_t&& line)
        {
            m_empty = false;

            if (m_buffer.size() < m_sizingBuffer)
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
            bool SpaceAfter = true;
        };

        Reporter& m_reporter;
        std::array<Column, FieldCount> m_columns;
        size_t m_sizingBuffer;
        std::vector<line_t> m_buffer;
        bool m_bufferEvaluated = false;
        bool m_empty = true;

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
                    m_columns[i].MaxLength = std::max(m_columns[i].MaxLength, Utility::UTF8ColumnWidth(line[i]));
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
                totalRequired += m_columns[i].MaxLength + (m_columns[i].SpaceAfter ? 1 : 0);
            }

            size_t consoleWidth = GetConsoleWidth();

            // If the total space would be too big, shrink them.
            // We don't want to use the last column, lest we auto-wrap
            if (totalRequired >= consoleWidth)
            {
                size_t extra = (totalRequired - consoleWidth) + 1;

                while (extra)
                {
                    size_t targetIndex = 0;
                    size_t targetVal = m_columns[0].MaxLength;
                    for (size_t j = 1; j < FieldCount; ++j)
                    {
                        if (m_columns[j].MaxLength > targetVal)
                        {
                            targetIndex = j;
                            targetVal = m_columns[j].MaxLength;
                        }
                    }
                    m_columns[targetIndex].MaxLength -= 1;
                    extra -= 1;
                }

                totalRequired = consoleWidth - 1;
            }

            // Header line
            line_t headerLine;

            for (size_t i = 0; i < FieldCount; ++i)
            {
                headerLine[i] = m_columns[i].Name.get();
            }

            OutputLineToStream(headerLine);

            m_reporter.Info() << std::wstring(totalRequired, '-') << std::endl;

            for (const auto& line : m_buffer)
            {
                OutputLineToStream(line);
            }

            m_bufferEvaluated = true;
        }

        void OutputLineToStream(const line_t& line)
        {
            auto out = m_reporter.Info();

            for (size_t i = 0; i < FieldCount; ++i)
            {
                const auto& col = m_columns[i];

                if (col.MaxLength)
                {
                    size_t valueLength = Utility::UTF8ColumnWidth(line[i]);

                    if (valueLength > col.MaxLength)
                    {
                        size_t actualWidth;
                        out << Utility::UTF8TrimRightToColumnWidth(line[i], col.MaxLength - 1, actualWidth) << "\xE2\x80\xA6"; // UTF8 encoding of ellipsis (…) character

                        // Some characters take 2 unit space, the trimmed string length might be 1 less than the expected length.
                        if (actualWidth != col.MaxLength - 1)
                        {
                            out << ' ';
                        }

                        if (col.SpaceAfter)
                        {
                            out << ' ';
                        }
                    }
                    else
                    {
                        out << line[i];

                        if (col.SpaceAfter)
                        {
                            out << std::wstring(col.MaxLength - valueLength + 1, ' ');
                        }
                    }
                }
            }

            out << std::endl;
        }
    };
}
*/