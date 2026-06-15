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
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "Reporter.h"
#include "VTSupport.h"

namespace wsl::windows::wslc {

using namespace wsl::windows::common::vt;

// Returns visible display width of a string, excluding VT escape sequences (CSI and OSC).
inline size_t GetVisibleWidth(const std::wstring& str)
{
    size_t width = 0;
    size_t i = 0;
    const size_t len = str.size();

    while (i < len)
    {
        if (str[i] == L'\x1b' && i + 1 < len)
        {
            const wchar_t next = str[i + 1];
            i += 2; // consume ESC + introducer

            if (next == L'[')
            {
                // CSI sequence: ESC [ <param bytes 0x30-0x3F>* <intermediate 0x20-0x2F>* <final 0x40-0x7E>
                while (i < len && str[i] >= 0x20 && str[i] <= 0x3F)
                {
                    ++i;
                }
                if (i < len && str[i] >= 0x40 && str[i] <= 0x7E)
                {
                    ++i;
                }
            }
            else if (next == L']')
            {
                // OSC sequence: ESC ] ... terminated by ST (ESC \) or BEL (0x07)
                while (i < len)
                {
                    if (str[i] == L'\x07')
                    {
                        ++i;
                        break;
                    }
                    if (str[i] == L'\x1b' && i + 1 < len && str[i + 1] == L'\\')
                    {
                        i += 2;
                        break;
                    }
                    ++i;
                }
            }
            // Any other ESC + byte: introducer already consumed, skip.
        }
        else
        {
            ++width;
            ++i;
        }
    }

    return width;
}

// Returns the raw character count, ignoring VT sequences.
inline size_t GetStringColumnWidth(const wchar_t* str)
{
    if (!str)
    {
        return 0;
    }
    return wcslen(str);
}

inline std::wstring TrimStringToColumnWidth(const wchar_t* str, size_t maxWidth, size_t& actualWidth)
{
    if (!str)
    {
        actualWidth = 0;
        return L"";
    }

    const size_t len = wcslen(str);
    if (len <= maxWidth)
    {
        actualWidth = len;
        return std::wstring(str);
    }

    actualWidth = maxWidth;
    return std::wstring(str, maxWidth);
}

// Parsed representation of a cell's VT sequences and visible content.
// All VT sequences appear before visible text; visibleText contains no sequences.
// leadingSeqs non-empty implies trailingReset is populated (enforced by ParseCell).
struct ParsedCell
{
    std::wstring leadingSeqs;   // All VT sequences before the first visible character.
    std::wstring visibleText;   // The visible content only, no sequences.
    std::wstring trailingReset; // The reset sequence at the end.
};

// Splits a cell into leading VT sequences, visible text, and trailing reset.
// Appends Format::Default when leading sequences are present but no trailing reset is found.
inline ParsedCell ParseCell(const std::wstring& cell, bool vtEnabled)
{
    if (!vtEnabled)
    {
        return {L"", cell, L""};
    }

    ParsedCell result;
    size_t i = 0;
    const size_t len = cell.size();

    // Consume all leading VT sequences before the first visible character.
    while (i < len && cell[i] == L'\x1b' && i + 1 < len)
    {
        const size_t seqStart = i;
        const wchar_t next = cell[i + 1];
        i += 2;

        if (next == L'[')
        {
            while (i < len && cell[i] >= 0x20 && cell[i] <= 0x3F)
            {
                ++i;
            }
            if (i < len && cell[i] >= 0x40 && cell[i] <= 0x7E)
            {
                ++i;
            }
        }
        else if (next == L']')
        {
            while (i < len)
            {
                if (cell[i] == L'\x07')
                {
                    ++i;
                    break;
                }
                if (cell[i] == L'\x1b' && i + 1 < len && cell[i + 1] == L'\\')
                {
                    i += 2;
                    break;
                }
                ++i;
            }
        }

        result.leadingSeqs += cell.substr(seqStart, i - seqStart);
    }

    if (result.leadingSeqs.empty())
    {
        // No leading sequences — the whole cell is plain visible text.
        result.visibleText = cell;
        return result;
    }

    // Strip trailing reset; recognise both Format::Default ("\x1b[0m") and short form ("\x1b[m").
    const std::wstring_view c_resetFull = Format::Default.Get();
    constexpr std::wstring_view c_resetShort{L"\x1b[m"};

    std::wstring_view remainder{cell.c_str() + i, len - i};

    if (remainder.size() >= c_resetFull.size() && remainder.substr(remainder.size() - c_resetFull.size()) == std::wstring_view{c_resetFull})
    {
        result.visibleText = std::wstring(remainder.substr(0, remainder.size() - c_resetFull.size()));
        result.trailingReset = c_resetFull;
    }
    else if (remainder.size() >= c_resetShort.size() && remainder.substr(remainder.size() - c_resetShort.size()) == c_resetShort)
    {
        result.visibleText = std::wstring(remainder.substr(0, remainder.size() - c_resetShort.size()));
        result.trailingReset = std::wstring(c_resetShort);
    }
    else
    {
        // No trailing reset found — enforce Format::Default to prevent color bleed.
        result.visibleText = std::wstring(remainder);
        result.trailingReset = Format::Default.Get();
    }

    return result;
}

// Controls how a column handles content that exceeds its available width.
enum class ColumnOverflow
{
    // Truncates content with an ellipsis at MaxWidth; column width is fixed and does not
    // participate in the shrink loop.
    Truncate,

    // Participates in the shrink loop: reduced largest-first down to MinWidth, then truncated.
    // PreferredShrink=true marks this as a higher-priority shrink target.
    Shrink,

    // Wraps long values across multiple physical rows; width is remaining space after other columns.
    Wrap,
};

struct ColumnWidthConfig
{
    static constexpr size_t NoLimit = 0;

    size_t MinWidth = NoLimit; // Minimum visible width (NoLimit = header width).
    size_t MaxWidth = NoLimit; // Maximum visible width cap (NoLimit = unlimited).
    ColumnOverflow Overflow = ColumnOverflow::Truncate;
    bool PreferredShrink = true; // Prioritizes this column in the shrink loop.
};

struct ColumnDefinition
{
    std::wstring Name;
    ColumnWidthConfig Config;
};

// TODO: Improve for use with sparse data.
template <size_t FieldCount>
struct TableOutput
{
    static_assert(FieldCount > 0, "TableOutput requires at least one column");

    using header_t = std::array<std::wstring, FieldCount>;
    using line_t = std::array<std::wstring, FieldCount>;
    using column_config_t = std::array<ColumnWidthConfig, FieldCount>;
    using column_def_t = std::array<ColumnDefinition, FieldCount>;

    static constexpr size_t DefaultColumnPadding = 3; // Docker-like spacing between columns

    // Generous fallback used when the destination is redirected (no real console width).
    // The wrap pass is skipped in that case so the receiver controls its own width.
    static constexpr size_t DefaultRedirectedConsoleWidth = 2000;

    TableOutput(Reporter& reporter, header_t&& header, size_t sizingBuffer = 50, size_t columnPadding = DefaultColumnPadding, Reporter::Level level = Reporter::Level::Output) :
        m_reporter(reporter),
        m_outputLevel(level),
        m_sizingBuffer(sizingBuffer),
        m_columnPadding(columnPadding),
        m_colorEnabled(reporter.IsColorEnabled())
    {
        InitializeColumns(std::move(header));
    }

    TableOutput(
        Reporter& reporter,
        header_t&& header,
        column_config_t&& config,
        size_t sizingBuffer = 50,
        size_t columnPadding = DefaultColumnPadding,
        Reporter::Level level = Reporter::Level::Output) :
        m_reporter(reporter),
        m_outputLevel(level),
        m_sizingBuffer(sizingBuffer),
        m_columnPadding(columnPadding),
        m_columnConfigs(std::move(config)),
        m_colorEnabled(reporter.IsColorEnabled())
    {
        InitializeColumns(std::move(header));
    }

    TableOutput(
        Reporter& reporter,
        column_def_t&& columns,
        size_t sizingBuffer = 50,
        size_t columnPadding = DefaultColumnPadding,
        Reporter::Level level = Reporter::Level::Output) :
        m_reporter(reporter),
        m_outputLevel(level),
        m_sizingBuffer(sizingBuffer),
        m_columnPadding(columnPadding),
        m_colorEnabled(reporter.IsColorEnabled())
    {
        header_t headers;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            headers[i] = std::move(columns[i].Name);
            m_columnConfigs[i] = columns[i].Config;
        }
        InitializeColumns(std::move(headers));
    }

    // Updates config store and Column state; safe to call after construction.
    void SetColumnConfig(size_t columnIndex, const ColumnWidthConfig& config)
    {
        if (columnIndex < FieldCount)
        {
            m_columnConfigs[columnIndex] = config;
            SyncColumnFromConfig(columnIndex);
        }
    }

    void SetAlwaysShowHeader(bool alwaysShow)
    {
        m_alwaysShowHeader = alwaysShow;
    }
    void SetShowHeader(bool showHeader)
    {
        m_showHeader = showHeader;
    }
    // Sets spaces prepended to every row. Does not affect column width calculations.
    void SetRowIndent(size_t spaces)
    {
        m_rowIndent = spaces;
    }

    // Enables VT-aware color formatting: excludes color sequences from column width
    // calculations and propagates formatting across wrapped and truncated rows.
    // Defaults to the Reporter's color setting at construction; call this to override.
    // Must be set before the first OutputLine() call.
    void SetColorEnabled(bool enabled)
    {
        m_colorEnabled = enabled;
    }
    // Overrides console width for column shrinking; pass 0 to restore default (Reporter-derived).
    // When set, the wrap pass also runs as if a real console were attached.
    void SetConsoleWidthOverride(size_t width)
    {
        m_consoleWidthOverride = width;
    }
    // Promotes all Truncate columns to Shrink globally; no effect on Wrap columns.
    void SetColumnWidthLimiting(bool enable)
    {
        for (size_t i = 0; i < FieldCount; ++i)
        {
            if (enable && m_columnConfigs[i].Overflow == ColumnOverflow::Truncate)
            {
                m_columnConfigs[i].Overflow = ColumnOverflow::Shrink;
                SyncColumnFromConfig(i);
            }
        }
    }

    void OutputLine(line_t&& line)
    {
        m_empty = false;

        // Buffer rows to ensure accurate column sizing before flush.
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
        else if (m_alwaysShowHeader && m_showHeader)
        {
            OutputHeaderOnly();
        }
    }

    bool IsEmpty()
    {
        return m_empty;
    }

private:
    struct Column
    {
        std::wstring Name;
        size_t MinLength = 0;
        size_t MaxLength = 0;
        size_t ConfiguredMaxLength = 0; // Max length from configuration
        bool SpaceAfter = true;
        ColumnOverflow Overflow = ColumnOverflow::Truncate;
    };

    Reporter& m_reporter;
    Reporter::Level m_outputLevel;
    std::array<Column, FieldCount> m_columns;
    column_config_t m_columnConfigs;
    size_t m_sizingBuffer;
    size_t m_columnPadding;
    size_t m_rowIndent = 0;
    bool m_colorEnabled;
    std::vector<line_t> m_buffer;
    bool m_bufferEvaluated = false;
    bool m_empty = true;
    bool m_alwaysShowHeader = true;
    bool m_showHeader = true;
    bool m_dropEmptyColumns = false;
    size_t m_consoleWidthOverride = 0;

    // Returns visible display width when VT is enabled, or raw character count otherwise.
    size_t CellWidth(const std::wstring& value) const
    {
        return m_colorEnabled ? GetVisibleWidth(value) : GetStringColumnWidth(value.c_str());
    }

    // Syncs Column state from m_columnConfigs[i]; call whenever a config entry changes.
    void SyncColumnFromConfig(size_t i)
    {
        auto& col = m_columns[i];
        const auto& cfg = m_columnConfigs[i];

        col.Overflow = cfg.Overflow;
        col.ConfiguredMaxLength = (cfg.MaxWidth != ColumnWidthConfig::NoLimit) ? cfg.MaxWidth : 0;

        if (cfg.MinWidth != ColumnWidthConfig::NoLimit)
        {
            col.MinLength = std::max(CellWidth(col.Name), cfg.MinWidth);
        }
        else
        {
            col.MinLength = CellWidth(col.Name);
        }
    }

    void InitializeColumns(header_t&& header)
    {
        for (size_t i = 0; i < FieldCount; ++i)
        {
            m_columns[i].Name = std::move(header[i]);
            m_columns[i].MaxLength = 0;
            SyncColumnFromConfig(i);
        }
    }

    // Returns the effective console width (in columns) of the destination, or std::nullopt
    // when the destination is redirected. SetConsoleWidthOverride() takes precedence and is
    // treated as a real console (the wrap pass uses has_value() to gate its behavior).
    std::optional<size_t> GetEffectiveConsoleWidth() const
    {
        if (m_consoleWidthOverride > 0)
        {
            return m_consoleWidthOverride;
        }

        if (const auto width = m_reporter.GetConsoleWidth(m_outputLevel); width.has_value())
        {
            return static_cast<size_t>(*width);
        }

        return std::nullopt;
    }

    // Splits visible text into word-boundary chunks of at most maxWidth chars.
    // Operates on plain visible text only — VT sequences must be stripped before calling.
    static std::vector<std::wstring> WrapText(const std::wstring& text, size_t maxWidth)
    {
        if (maxWidth == 0 || text.length() <= maxWidth)
        {
            return {text};
        }

        std::vector<std::wstring> lines;
        size_t pos = 0;

        while (pos < text.length())
        {
            size_t chunkEnd = std::min(pos + maxWidth, text.length());

            if (chunkEnd < text.length())
            {
                size_t breakAt = text.rfind(L' ', chunkEnd);
                if (breakAt != std::wstring::npos && breakAt > pos)
                {
                    chunkEnd = breakAt;
                }
                // No space found within the chunk — hard-break at maxWidth.
            }

            lines.emplace_back(text.substr(pos, chunkEnd - pos));

            pos = chunkEnd;
            while (pos < text.length() && text[pos] == L' ')
            {
                ++pos;
            }
        }

        return lines;
    }

    // Returns per-row cell values for a wrapping column; each VT chunk carries the original sequences.
    std::vector<std::wstring> BuildWrappedCells(const std::wstring& cellValue, const Column& col) const
    {
        if (col.Overflow != ColumnOverflow::Wrap || col.MaxLength == 0)
        {
            // Not a wrapping column — return as a single-element vector.
            return {cellValue};
        }

        if (!m_colorEnabled)
        {
            return WrapText(cellValue, col.MaxLength);
        }

        // VT-aware wrap: operate on visible text, reassemble with sequences.
        const ParsedCell parsed = ParseCell(cellValue, true);
        const auto chunks = WrapText(parsed.visibleText, col.MaxLength);

        std::vector<std::wstring> result;
        result.reserve(chunks.size());
        for (const auto& chunk : chunks)
        {
            result.push_back(parsed.leadingSeqs + chunk + parsed.trailingReset);
        }
        return result;
    }

    void OutputHeaderOnly()
    {
        for (size_t i = 0; i < FieldCount; ++i)
        {
            m_columns[i].MaxLength = m_columns[i].MinLength;
        }

        m_columns[FieldCount - 1].SpaceAfter = false;

        line_t headerLine;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            headerLine[i] = m_columns[i].Name;
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

        // Determine the maximum visible width for each column across all buffered rows.
        for (const auto& line : m_buffer)
        {
            for (size_t i = 0; i < FieldCount; ++i)
            {
                size_t w = CellWidth(line[i]);

                if (m_columns[i].ConfiguredMaxLength != ColumnWidthConfig::NoLimit)
                {
                    w = std::min(w, m_columns[i].ConfiguredMaxLength);
                }

                m_columns[i].MaxLength = std::max(m_columns[i].MaxLength, w);
            }
        }

        // Apply MinLength so empty columns still render at least as wide as their header.
        for (size_t i = 0; i < FieldCount; ++i)
        {
            if (m_columns[i].MaxLength || !m_dropEmptyColumns)
            {
                m_columns[i].MaxLength = std::max(m_columns[i].MaxLength, m_columns[i].MinLength);
            }
        }

        // Last column never needs trailing padding.
        m_columns[FieldCount - 1].SpaceAfter = false;

        // Disable SpaceAfter on columns that are followed only by empty columns.
        for (size_t i = FieldCount - 1; i > 0; --i)
        {
            if (m_columns[i].MaxLength)
            {
                break;
            }
            m_columns[i - 1].SpaceAfter = false;
        }

        // Compute total visible width required to not truncate any columns.
        size_t totalRequired = 0;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            totalRequired += m_columns[i].MaxLength + (m_columns[i].SpaceAfter ? m_columnPadding : 0);
        }

        const auto consoleWidthOpt = GetEffectiveConsoleWidth();
        const size_t consoleWidth = consoleWidthOpt.value_or(DefaultRedirectedConsoleWidth);
        // The row indent consumes display space that is not part of any column.
        const size_t availableWidth = (consoleWidth > m_rowIndent) ? consoleWidth - m_rowIndent : 0;

        // Shrink pass: reduce Shrink columns until the total fits within the available width.
        if (totalRequired > availableWidth)
        {
            size_t extra = totalRequired - availableWidth;

            while (extra > 0)
            {
                // Find the largest shrinkable Shrink column, preferring PreferredShrink ones.
                size_t targetIndex = FieldCount; // sentinel: no target found yet
                size_t targetVal = 0;

                for (size_t j = 0; j < FieldCount; ++j)
                {
                    if (m_columns[j].Overflow != ColumnOverflow::Shrink)
                    {
                        continue;
                    }
                    if (m_columns[j].MaxLength <= m_columns[j].MinLength)
                    {
                        continue;
                    }

                    const bool isPreferred = m_columnConfigs[j].PreferredShrink;
                    const bool currentPreferred = (targetIndex < FieldCount) ? m_columnConfigs[targetIndex].PreferredShrink : false;

                    if (targetIndex == FieldCount || (isPreferred && !currentPreferred) ||
                        (isPreferred == currentPreferred && m_columns[j].MaxLength > targetVal))
                    {
                        targetIndex = j;
                        targetVal = m_columns[j].MaxLength;
                    }
                }

                if (targetIndex == FieldCount)
                {
                    break;
                } // nothing shrinkable — stop

                m_columns[targetIndex].MaxLength -= 1;
                extra -= 1;
            }
        }

        // Wrap pass: clamp each Wrap column to remaining space after all other columns.
        // Skipped when the destination is redirected so the receiver controls its own width.
        if (consoleWidthOpt.has_value())
        {
            for (size_t i = 0; i < FieldCount; ++i)
            {
                if (m_columns[i].Overflow != ColumnOverflow::Wrap || !m_columns[i].MaxLength)
                {
                    continue;
                }

                // Sum the width consumed by every other column including its inter-column padding.
                size_t otherWidth = 0;
                for (size_t j = 0; j < FieldCount; ++j)
                {
                    if (j != i)
                    {
                        otherWidth += m_columns[j].MaxLength + (m_columns[j].SpaceAfter ? m_columnPadding : 0);
                    }
                }
                // Account for the padding that follows this column (if any).
                if (m_columns[i].SpaceAfter)
                {
                    otherWidth += m_columnPadding;
                }

                const size_t wrapBudget = (availableWidth > otherWidth) ? availableWidth - otherWidth : 1; // always allow at least one character

                if (m_columns[i].MaxLength > wrapBudget)
                {
                    m_columns[i].MaxLength = std::max(wrapBudget, m_columns[i].MinLength);
                }
            }
        }

        if (m_showHeader)
        {
            line_t headerLine;
            for (size_t i = 0; i < FieldCount; ++i)
            {
                headerLine[i] = m_columns[i].Name;
            }
            OutputLineToStream(headerLine);
        }

        for (const auto& line : m_buffer)
        {
            OutputLineToStream(line);
        }

        m_bufferEvaluated = true;
    }

    // Renders a logical row, emitting multiple physical rows for word-wrapping columns.
    void OutputLineToStream(const line_t& line)
    {
        size_t physicalRows = 1;
        std::array<std::vector<std::wstring>, FieldCount> wrappedCells;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            wrappedCells[i] = BuildWrappedCells(line[i], m_columns[i]);
            physicalRows = std::max(physicalRows, wrappedCells[i].size());
        }

        for (size_t row = 0; row < physicalRows; ++row)
        {
            std::wstringstream stream;

            // Prepend row indent before the first column.
            if (m_rowIndent > 0)
            {
                stream << std::wstring(m_rowIndent, L' ');
            }

            for (size_t i = 0; i < FieldCount; ++i)
            {
                const auto& col = m_columns[i];
                if (!col.MaxLength)
                {
                    continue;
                }

                // On continuation rows (row > 0) exhausted columns render as blank.
                const std::wstring& cellValue = (row < wrappedCells[i].size()) ? wrappedCells[i][row] : L"";
                const size_t valueLength = CellWidth(cellValue);

                if (col.Overflow != ColumnOverflow::Wrap && valueLength > col.MaxLength)
                {
                    // Non-wrapping column exceeds budget: truncate visible text and append ellipsis.
                    if (m_colorEnabled)
                    {
                        const ParsedCell parsed = ParseCell(cellValue, true);
                        size_t actualWidth;
                        stream << parsed.leadingSeqs
                               << TrimStringToColumnWidth(parsed.visibleText.c_str(), col.MaxLength - 1, actualWidth) << L"\u2026";
                        if (!parsed.trailingReset.empty())
                        {
                            stream << parsed.trailingReset;
                        }
                    }
                    else
                    {
                        size_t actualWidth;
                        stream << TrimStringToColumnWidth(cellValue.c_str(), col.MaxLength - 1, actualWidth) << L"\u2026";

                        // Wide chars may consume 2 columns; pad if trimmed length is 1 short.
                        if (actualWidth != col.MaxLength - 1)
                        {
                            stream << L' ';
                        }
                    }

                    if (col.SpaceAfter)
                    {
                        stream << std::wstring(m_columnPadding, L' ');
                    }
                }
                else
                {
                    stream << cellValue;

                    if (col.SpaceAfter)
                    {
                        stream << std::wstring(col.MaxLength - valueLength + m_columnPadding, L' ');
                    }
                }
            }

            m_reporter.GetWriter(m_outputLevel) << stream.str() << std::endl;
        }
    }
};

} // namespace wsl::windows::wslc
