/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableOutput.h

Abstract:

    Structured table output for the WSLC CLI. Cells are either plain text or
    format-string + Sequence args. Sequences are zero display width; the table
    measures visible width by counting non-placeholder characters. At render
    time, sequences are emitted or stripped based on Reporter color state.

--*/
#pragma once

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <wil/result_macros.h>
#include "Reporter.h"
#include "VTSupport.h"

namespace wsl::windows::wslc {

using wsl::windows::common::vt::Sequence;

// A table cell: either plain text or a format string with Sequence placeholders.
// Every {} in the format string corresponds to a Sequence (zero display width).
// Visible width is the count of non-placeholder characters in the format string.
struct FormattedCell
{
    std::wstring fmt;
    std::vector<const Sequence*> sequences;

    // Default constructor — empty cell.
    FormattedCell() = default;

    // Implicit from wstring — plain text cell (no formatting).
    FormattedCell(std::wstring text) : fmt(std::move(text))
    {
    }

    // Implicit from wstring_view.
    FormattedCell(std::wstring_view text) : fmt(text)
    {
    }

    // Implicit from literal.
    FormattedCell(const wchar_t* text) : fmt(text)
    {
    }

    // Formatted cell: format string with Sequence placeholders.
    FormattedCell(std::wstring format, std::initializer_list<const Sequence*> seqs) : fmt(std::move(format)), sequences(seqs)
    {
    }

    // Single-sequence cell: wraps text with the sequence and a trailing reset.
    FormattedCell(std::wstring_view text, const Sequence& seq);

    // Block temporaries: the cell only stores a pointer to seq, so binding a Sequence rvalue (including
    // derived types such as the ConstructedSequence returned by Sgr()) would dangle once the full
    // expression ends. Only long-lived Sequence instances may be used here.
    FormattedCell(std::wstring_view text, const Sequence&& seq) = delete;

    // Visible width: count characters that are not part of {} placeholders.
    size_t VisibleWidth() const;

    // Renders the cell with or without sequences.
    // When vtEnabled is false, all {} placeholders are skipped (no VT output).
    // When vtEnabled is true but colorEnabled is false, only non-color sequences are emitted.
    // When both are true, all sequences are emitted.
    std::wstring Render(bool vtEnabled, bool colorEnabled) const;

    // Renders with visible text truncated to maxWidth characters, appending ellipsis.
    // Sequences after the truncation point are still emitted (for resets).
    std::wstring RenderTruncated(size_t maxWidth, bool vtEnabled, bool colorEnabled) const;
};

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

namespace details {

    // Splits visible text into word-boundary chunks of at most maxWidth chars.
    std::vector<std::wstring> WrapText(const std::wstring& text, size_t maxWidth);

} // namespace details

template <size_t FieldCount>
struct TableOutput
{
    static_assert(FieldCount > 0, "TableOutput requires at least one column");

    using header_t = std::array<std::wstring, FieldCount>;
    using line_t = std::array<FormattedCell, FieldCount>;
    using column_config_t = std::array<ColumnWidthConfig, FieldCount>;
    using column_def_t = std::array<ColumnDefinition, FieldCount>;

    static constexpr size_t DefaultColumnPadding = 3; // Docker-like spacing between columns

    // Generous fallback used when the destination is redirected (no real console width).
    // The wrap pass is skipped in that case so the receiver controls its own width.
    static constexpr size_t DefaultRedirectedConsoleWidth = 2000;

    TableOutput(Reporter& reporter, header_t&& header, size_t sizingBuffer = 50, size_t columnPadding = DefaultColumnPadding, Reporter::Level level = Reporter::Level::Output) :
        m_reporter(reporter),
        m_outputLevel(level),
        m_vtEnabled(reporter.IsVTEnabled(level)),
        m_colorEnabled(reporter.IsColorEnabled(level)),
        m_sizingBuffer(sizingBuffer),
        m_columnPadding(columnPadding)
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
        m_vtEnabled(reporter.IsVTEnabled(level)),
        m_colorEnabled(reporter.IsColorEnabled(level)),
        m_sizingBuffer(sizingBuffer),
        m_columnPadding(columnPadding),
        m_columnConfigs(std::move(config))
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
        m_vtEnabled(reporter.IsVTEnabled(level)),
        m_colorEnabled(reporter.IsColorEnabled(level)),
        m_sizingBuffer(sizingBuffer),
        m_columnPadding(columnPadding)
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

    // Overrides console width for column shrinking; pass 0 to restore default (Reporter-derived).
    // When set, the wrap pass also runs as if a real console were attached.
    void SetConsoleWidthOverride(size_t width)
    {
        m_consoleWidthOverride = width;
    }

    void WriteRow(line_t&& line)
    {
        m_empty = false;

        // Buffer rows to size columns before flush. When every column is unbounded (no MaxWidth cap
        // and no Wrap/Shrink overflow), buffer all rows so column widths grow to fit the widest value
        // regardless of row order. With an overflow policy in play, cap the buffer at m_sizingBuffer
        // and stream the remainder to bound memory for large result sets.
        if (m_dataRowCount < m_sizingBuffer || AllColumnsUnbounded())
        {
            m_buffer.emplace_back(std::move(line));
            ++m_dataRowCount;
        }
        else
        {
            EvaluateAndFlushBuffer();
            OutputLineToStream(line);
        }
    }

    // Emits a standalone text line that does not participate in column sizing.
    // Use for section headers or blank separators between data rows.
    void WriteLine(FormattedCell cell = {})
    {
        m_empty = false;

        if (!m_bufferEvaluated)
        {
            m_buffer.emplace_back(std::move(cell));
        }
        else
        {
            OutputCellLineToStream(cell);
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
    // A break entry is a FormattedCell rendered as a standalone line (section header or blank).
    using buffer_entry_t = std::variant<line_t, FormattedCell>;

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
    const bool m_vtEnabled;
    const bool m_colorEnabled;
    std::array<Column, FieldCount> m_columns;
    column_config_t m_columnConfigs;
    size_t m_sizingBuffer;
    size_t m_columnPadding;
    size_t m_rowIndent = 0;
    std::vector<buffer_entry_t> m_buffer;
    size_t m_dataRowCount = 0;
    bool m_bufferEvaluated = false;
    bool m_empty = true;
    bool m_alwaysShowHeader = true;
    bool m_showHeader = true;
    bool m_dropEmptyColumns = false;
    size_t m_consoleWidthOverride = 0;

    // True when no column constrains its width (no MaxWidth cap and no Wrap/Shrink overflow).
    // Such tables buffer every row so a late, wide value is never truncated or misaligned.
    bool AllColumnsUnbounded() const
    {
        for (size_t i = 0; i < FieldCount; ++i)
        {
            if (m_columns[i].ConfiguredMaxLength != 0 || m_columns[i].Overflow != ColumnOverflow::Truncate)
            {
                return false;
            }
        }
        return true;
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
            col.MinLength = std::max(col.Name.size(), cfg.MinWidth);
        }
        else
        {
            col.MinLength = col.Name.size();
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

    // Wraps a cell's visible text into chunks, preserving formatting on each chunk.
    std::vector<FormattedCell> BuildWrappedCells(const FormattedCell& cell, const Column& col) const
    {
        if (col.Overflow != ColumnOverflow::Wrap || col.MaxLength == 0)
        {
            return {cell};
        }

        // Extract the visible text for wrapping.
        const size_t visWidth = cell.VisibleWidth();
        if (visWidth <= col.MaxLength)
        {
            return {cell};
        }

        // For plain cells, wrap the text directly.
        if (cell.sequences.empty())
        {
            auto chunks = details::WrapText(cell.fmt, col.MaxLength);
            std::vector<FormattedCell> result;
            result.reserve(chunks.size());
            for (auto& chunk : chunks)
            {
                result.emplace_back(std::move(chunk));
            }
            return result;
        }

        // Wrapping only supports single-style cells (open + reset). Complex cells with
        // multiple sequences (e.g., hyperlinks with distinct open/close pairs) cannot be
        // reliably split across wrapped lines. Callers needing rich formatting in a wrapped
        // column should use a single constructed Sequence that combines all escape codes.
        THROW_HR_IF(E_INVALIDARG, cell.sequences.size() > 2);

        // For formatted cells, extract visible text, wrap it, then re-apply formatting.
        std::wstring visibleText;
        visibleText.reserve(visWidth);
        for (size_t i = 0; i < cell.fmt.size(); ++i)
        {
            if (i + 1 < cell.fmt.size() && cell.fmt[i] == L'{' && cell.fmt[i + 1] == L'}')
            {
                ++i;
            }
            else
            {
                visibleText += cell.fmt[i];
            }
        }

        auto chunks = details::WrapText(visibleText, col.MaxLength);
        std::vector<FormattedCell> result;
        result.reserve(chunks.size());
        for (auto& chunk : chunks)
        {
            result.emplace_back(FormattedCell(std::wstring_view{chunk}, *cell.sequences.front()));
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
            headerLine[i] = FormattedCell(m_columns[i].Name);
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

        // Determine the maximum visible width for each column across all buffered data rows.
        for (const auto& entry : m_buffer)
        {
            const auto* line = std::get_if<line_t>(&entry);
            if (!line)
            {
                continue;
            }

            for (size_t i = 0; i < FieldCount; ++i)
            {
                size_t w = (*line)[i].VisibleWidth();

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
        const size_t availableWidth = (consoleWidth > m_rowIndent) ? consoleWidth - m_rowIndent : 0;

        // Shrink pass: reduce Shrink columns until the total fits within the available width.
        if (totalRequired > availableWidth)
        {
            size_t extra = totalRequired - availableWidth;

            while (extra > 0)
            {
                size_t targetIndex = FieldCount;
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
                }

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

                size_t otherWidth = 0;
                for (size_t j = 0; j < FieldCount; ++j)
                {
                    if (j != i)
                    {
                        otherWidth += m_columns[j].MaxLength + (m_columns[j].SpaceAfter ? m_columnPadding : 0);
                    }
                }
                if (m_columns[i].SpaceAfter)
                {
                    otherWidth += m_columnPadding;
                }

                const size_t wrapBudget = (availableWidth > otherWidth) ? availableWidth - otherWidth : 1;

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
                headerLine[i] = FormattedCell(m_columns[i].Name);
            }
            OutputLineToStream(headerLine);
        }

        for (const auto& entry : m_buffer)
        {
            if (const auto* line = std::get_if<line_t>(&entry))
            {
                OutputLineToStream(*line);
            }
            else if (const auto* cell = std::get_if<FormattedCell>(&entry))
            {
                OutputCellLineToStream(*cell);
            }
        }

        m_bufferEvaluated = true;
    }

    void OutputCellLineToStream(const FormattedCell& cell)
    {
        m_reporter.Write(m_outputLevel, L"{}\n", cell.Render(m_vtEnabled, m_colorEnabled));
    }

    // Renders a logical row, emitting multiple physical rows for word-wrapping columns.
    void OutputLineToStream(const line_t& line)
    {
        size_t physicalRows = 1;
        std::array<std::vector<FormattedCell>, FieldCount> wrappedCells;
        for (size_t i = 0; i < FieldCount; ++i)
        {
            wrappedCells[i] = BuildWrappedCells(line[i], m_columns[i]);
            physicalRows = std::max(physicalRows, wrappedCells[i].size());
        }

        for (size_t row = 0; row < physicalRows; ++row)
        {
            std::wstring rowStr;

            if (m_rowIndent > 0)
            {
                rowStr.append(m_rowIndent, L' ');
            }

            for (size_t i = 0; i < FieldCount; ++i)
            {
                const auto& col = m_columns[i];
                if (!col.MaxLength)
                {
                    continue;
                }

                // On continuation rows, exhausted columns render as blank.
                static const FormattedCell emptyCell{L""};
                const FormattedCell& cell = (row < wrappedCells[i].size()) ? wrappedCells[i][row] : emptyCell;
                const size_t valueLength = cell.VisibleWidth();

                if (col.Overflow != ColumnOverflow::Wrap && valueLength > col.MaxLength)
                {
                    // Truncate and append ellipsis.
                    rowStr.append(cell.RenderTruncated(col.MaxLength, m_vtEnabled, m_colorEnabled));

                    if (col.SpaceAfter)
                    {
                        rowStr.append(m_columnPadding, L' ');
                    }
                }
                else
                {
                    rowStr.append(cell.Render(m_vtEnabled, m_colorEnabled));

                    if (col.SpaceAfter)
                    {
                        rowStr.append(col.MaxLength - valueLength + m_columnPadding, L' ');
                    }
                }
            }

            m_reporter.Write(m_outputLevel, L"{}\n", rowStr);
        }
    }
};

} // namespace wsl::windows::wslc
