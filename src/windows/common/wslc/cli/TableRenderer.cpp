/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableRenderer.cpp

Abstract:

    Implementation of table layout and emission.

--*/
#include "precomp.h"
#include "TableRenderer.h"

using namespace wsl::windows::common::vt;

namespace wsl::windows::wslc::cli {

namespace {

    struct RenderColumn
    {
        size_t Width = 0;         // Visible width the column renders at.
        size_t MinWidth = 0;      // Floor applied during fitting.
        size_t ConfiguredMax = 0; // Cap from configuration; 0 means unlimited.
        ColumnOverflow Overflow = ColumnOverflow::Truncate;
        bool PreferredShrink = true;
        bool SpaceAfter = true;
    };

    // Splits a cell into the chunks the column can hold. Every chunk keeps the cell's sequences.
    std::vector<Cell> WrapCell(const Cell& cell, const RenderColumn& column)
    {
        if (column.Overflow != ColumnOverflow::Wrap || column.Width == 0 || cell.VisibleWidth() <= column.Width)
        {
            return {cell};
        }

        std::vector<Cell> chunks;
        for (auto& text : details::WrapText(cell.Text, column.Width))
        {
            auto& chunk = chunks.emplace_back(std::move(text));
            chunk.Prefix = cell.Prefix;
            chunk.Suffix = cell.Suffix;
        }

        return chunks;
    }

    std::optional<size_t> GetEffectiveConsoleWidth(const Terminal& terminal, const TableData& table, Terminal::Level level)
    {
        if (table.ConsoleWidthOverride > 0)
        {
            return table.ConsoleWidthOverride;
        }

        if (const auto width = terminal.GetConsoleWidth(level); width.has_value())
        {
            return static_cast<size_t>(*width);
        }

        return std::nullopt;
    }

    // Establishes each column's floor and cap, then grows it to fit the widest value it holds.
    std::vector<RenderColumn> MeasureColumns(const TableData& table)
    {
        const size_t columnCount = table.Columns.size();
        std::vector<RenderColumn> columns(columnCount);

        for (size_t i = 0; i < columnCount; ++i)
        {
            const auto& config = table.Columns[i].Config;
            auto& column = columns[i];

            column.Overflow = config.Overflow;
            column.PreferredShrink = config.PreferredShrink;
            column.ConfiguredMax = (config.MaxWidth != ColumnWidthConfig::NoLimit) ? config.MaxWidth : 0;
            column.MinWidth = table.Columns[i].Name.size();

            if (config.MinWidth != ColumnWidthConfig::NoLimit)
            {
                column.MinWidth = std::max(column.MinWidth, config.MinWidth);
            }

            // The final column emits no trailing padding, so a minimum cell width there would only
            // produce trailing whitespace.
            if (i + 1 < columnCount && table.MinCellWidth > table.ColumnPadding)
            {
                column.MinWidth = std::max(column.MinWidth, table.MinCellWidth - table.ColumnPadding);
            }
        }

        for (const auto& row : table.Rows)
        {
            if (row.Spanning)
            {
                continue;
            }

            for (size_t i = 0; i < columnCount && i < row.Cells.size(); ++i)
            {
                size_t width = row.Cells[i].VisibleWidth();

                if (columns[i].ConfiguredMax != 0)
                {
                    width = std::min(width, columns[i].ConfiguredMax);
                }

                columns[i].Width = std::max(columns[i].Width, width);
            }
        }

        for (auto& column : columns)
        {
            column.Width = std::max(column.Width, column.MinWidth);

            // A column with nothing to show emits neither a value nor padding, so it must not
            // reserve any width.
            if (column.Width == 0)
            {
                column.SpaceAfter = false;
            }
        }

        columns.back().SpaceAfter = false;

        // Drop padding on columns followed only by empty ones so rows carry no trailing whitespace.
        for (size_t i = columnCount - 1; i > 0; --i)
        {
            if (columns[i].Width != 0)
            {
                break;
            }

            columns[i - 1].SpaceAfter = false;
        }

        return columns;
    }

    // Reduces Shrink columns largest-first until the table fits the available width.
    void ShrinkColumns(std::vector<RenderColumn>& columns, size_t availableWidth, size_t columnPadding)
    {
        size_t totalRequired = 0;
        for (const auto& column : columns)
        {
            totalRequired += column.Width + (column.SpaceAfter ? columnPadding : 0);
        }

        if (totalRequired <= availableWidth)
        {
            return;
        }

        size_t extra = totalRequired - availableWidth;
        while (extra > 0)
        {
            size_t targetIndex = columns.size();
            size_t targetWidth = 0;

            for (size_t i = 0; i < columns.size(); ++i)
            {
                if (columns[i].Overflow != ColumnOverflow::Shrink || columns[i].Width <= columns[i].MinWidth)
                {
                    continue;
                }

                const bool isPreferred = columns[i].PreferredShrink;
                const bool currentPreferred = (targetIndex < columns.size()) ? columns[targetIndex].PreferredShrink : false;

                if (targetIndex == columns.size() || (isPreferred && !currentPreferred) ||
                    (isPreferred == currentPreferred && columns[i].Width > targetWidth))
                {
                    targetIndex = i;
                    targetWidth = columns[i].Width;
                }
            }

            if (targetIndex == columns.size())
            {
                break;
            }

            columns[targetIndex].Width -= 1;
            extra -= 1;
        }
    }

    // Clamps each Wrap column to the space left over once every other column is placed.
    void FitWrapColumns(std::vector<RenderColumn>& columns, size_t availableWidth, size_t columnPadding)
    {
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (columns[i].Overflow != ColumnOverflow::Wrap || columns[i].Width == 0)
            {
                continue;
            }

            size_t otherWidth = columns[i].SpaceAfter ? columnPadding : 0;
            for (size_t j = 0; j < columns.size(); ++j)
            {
                if (j != i)
                {
                    otherWidth += columns[j].Width + (columns[j].SpaceAfter ? columnPadding : 0);
                }
            }

            const size_t wrapBudget = (availableWidth > otherWidth) ? availableWidth - otherWidth : 1;

            if (columns[i].Width > wrapBudget)
            {
                columns[i].Width = std::max(wrapBudget, columns[i].MinWidth);
            }
        }
    }

} // namespace

void RenderTable(Terminal& terminal, const TableData& table, Terminal::Level level)
{
    if (table.Columns.empty())
    {
        return;
    }

    if (table.Rows.empty() && !table.ShowHeader)
    {
        return;
    }

    const bool vtEnabled = terminal.IsVTEnabled(level);
    const bool colorEnabled = terminal.IsColorEnabled(level);

    auto columns = MeasureColumns(table);

    const auto consoleWidth = GetEffectiveConsoleWidth(terminal, table, level);
    const size_t totalWidth = consoleWidth.value_or(c_redirectedConsoleWidth);
    const size_t availableWidth = (totalWidth > table.RowIndent) ? totalWidth - table.RowIndent : 0;

    ShrinkColumns(columns, availableWidth, table.ColumnPadding);

    // Skipped when the destination is redirected so the receiver controls its own width.
    if (consoleWidth.has_value())
    {
        FitWrapColumns(columns, availableWidth, table.ColumnPadding);
    }

    const auto emitRow = [&](const std::vector<Cell>& cells) {
        size_t physicalRows = 1;
        std::vector<std::vector<Cell>> wrapped(columns.size());

        for (size_t i = 0; i < columns.size(); ++i)
        {
            wrapped[i] = (i < cells.size()) ? WrapCell(cells[i], columns[i]) : std::vector<Cell>{Cell{}};
            physicalRows = std::max(physicalRows, wrapped[i].size());
        }

        for (size_t physicalRow = 0; physicalRow < physicalRows; ++physicalRow)
        {
            std::wstring line;

            if (table.RowIndent > 0)
            {
                line.append(table.RowIndent, L' ');
            }

            for (size_t i = 0; i < columns.size(); ++i)
            {
                const auto& column = columns[i];
                if (column.Width == 0)
                {
                    continue;
                }

                // On continuation rows, exhausted columns render as blank.
                static const Cell emptyCell{};
                const Cell& cell = (physicalRow < wrapped[i].size()) ? wrapped[i][physicalRow] : emptyCell;
                const size_t valueLength = cell.VisibleWidth();

                if (column.Overflow != ColumnOverflow::Wrap && valueLength > column.Width)
                {
                    line.append(cell.RenderTruncated(column.Width, vtEnabled, colorEnabled));

                    if (column.SpaceAfter)
                    {
                        line.append(table.ColumnPadding, L' ');
                    }
                }
                else
                {
                    line.append(cell.Render(vtEnabled, colorEnabled));

                    if (column.SpaceAfter)
                    {
                        line.append(column.Width - valueLength + table.ColumnPadding, L' ');
                    }
                }
            }

            terminal.Write(level, L"{}\n", line);
        }
    };

    if (table.ShowHeader)
    {
        std::vector<Cell> headerCells;
        headerCells.reserve(table.Columns.size());
        for (const auto& column : table.Columns)
        {
            headerCells.emplace_back(column.Name);
        }

        emitRow(headerCells);
    }

    for (const auto& row : table.Rows)
    {
        if (row.Spanning)
        {
            static const Cell emptyCell{};
            const Cell& cell = row.Cells.empty() ? emptyCell : row.Cells.front();
            terminal.Write(level, L"{}\n", cell.Render(vtEnabled, colorEnabled));
        }
        else
        {
            emitRow(row.Cells);
        }
    }
}

} // namespace wsl::windows::wslc::cli
