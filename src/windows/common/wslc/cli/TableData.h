/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableData.h

Abstract:

    Data model for tabular CLI output. A table is a list of column definitions
    plus a list of rows; a row holds one cell per column, or a single spanning
    cell that occupies the whole line.

    Column widths are measured across every row, so a table must hold the
    complete result set before it is rendered. See TableRenderer.h for layout
    and emission.

--*/
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wsl::windows::common::vt {
struct Sequence;
}

namespace wsl::windows::cli::table {

using wsl::windows::common::vt::Sequence;

// IsColor marks the sequences that --no-color suppresses.
struct CellSequence
{
    std::wstring Text;
    bool IsColor = false;

    bool Empty() const
    {
        return Text.empty();
    }
};

// Prefix and Suffix are emitted around Text when the destination supports VT; they contribute no
// display width.
struct Cell
{
    std::wstring Text;
    CellSequence Prefix;
    CellSequence Suffix;

    Cell() = default;

    Cell(std::wstring text) : Text(std::move(text))
    {
    }

    Cell(std::wstring_view text) : Text(text)
    {
    }

    Cell(const wchar_t* text) : Text(text)
    {
    }

    // Wraps the text with the sequence and a trailing reset.
    Cell(std::wstring_view text, const Sequence& style);

    Cell(std::wstring_view text, const Sequence& prefix, const Sequence& suffix);

    size_t VisibleWidth() const
    {
        return Text.size();
    }

    // When vtEnabled is false no sequences are emitted; when only colorEnabled is false, the
    // color sequences are dropped.
    std::wstring Render(bool vtEnabled, bool colorEnabled) const;

    // Truncates the text to maxWidth characters with an ellipsis, still emitting the sequences
    // so styling is closed correctly.
    std::wstring RenderTruncated(size_t maxWidth, bool vtEnabled, bool colorEnabled) const;
};

// A normal row holds one cell per column. A spanning row holds a single cell emitted as a
// standalone line, taking no part in column sizing and receiving no indent.
struct Row
{
    std::vector<Cell> Cells;
    bool Spanning = false;

    // For rows whose shape varies with the column set. The cell count is validated by
    // TableData::AddRow.
    Row& AddCell(Cell cell)
    {
        Cells.emplace_back(std::move(cell));
        return *this;
    }

    Row& AddCellIf(bool condition, Cell cell)
    {
        return condition ? AddCell(std::move(cell)) : *this;
    }
};

// Controls how a column handles content that exceeds its available width.
enum class ColumnOverflow
{
    // Fixed width; truncates with an ellipsis at MaxWidth.
    Truncate,

    // Shrinks largest-first down to MinWidth, then truncates. PreferredShrink marks this as a
    // higher-priority shrink target.
    Shrink,

    // Takes the space remaining after the other columns and wraps across physical rows.
    Wrap,
};

struct ColumnWidthConfig
{
    static constexpr size_t NoLimit = 0;

    size_t MinWidth = NoLimit;
    size_t MaxWidth = NoLimit;
    ColumnOverflow Overflow = ColumnOverflow::Truncate;
    bool PreferredShrink = true;
};

struct ColumnDefinition
{
    std::wstring Name;
    ColumnWidthConfig Config;
};

inline constexpr size_t c_defaultColumnPadding = 3;

// Minimum total width of a column including its padding. The final column is exempt.
inline constexpr size_t c_defaultMinCellWidth = 10;

// The column set is fixed once the first row is added, so every row keeps a matching cell count.
// Only each column's width configuration stays mutable.
class TableData
{
public:
    bool ShowHeader = true;

    // Spaces prepended to every non-spanning row. Does not affect column width calculations.
    size_t RowIndent = 0;

    size_t ColumnPadding = c_defaultColumnPadding;
    size_t MinCellWidth = c_defaultMinCellWidth;

    TableData() = default;

    explicit TableData(std::vector<ColumnDefinition> columns) : m_columns(std::move(columns))
    {
    }

    TableData(std::initializer_list<std::wstring_view> headers);

    void Reserve(size_t rowCount)
    {
        m_rows.reserve(rowCount);
    }

    // Clears the width configuration of every subsequently added column.
    TableData& Truncate(bool enabled)
    {
        m_truncate = enabled;
        return *this;
    }

    // Only valid before the first row is added.
    TableData& AddColumn(std::wstring name, ColumnWidthConfig config = {});

    TableData& AddColumnIf(bool condition, std::wstring name, ColumnWidthConfig config = {})
    {
        return condition ? AddColumn(std::move(name), config) : *this;
    }

    // The cell count must match the column count.
    void AddRow(std::vector<Cell> cells);

    void AddRow(Row row);

    void AddSpanningRow(Cell cell = {});

    const std::vector<ColumnDefinition>& GetColumns() const
    {
        return m_columns;
    }

    const std::vector<Row>& GetRows() const
    {
        return m_rows;
    }

    ColumnWidthConfig& ColumnConfig(size_t index)
    {
        return m_columns.at(index).Config;
    }

    size_t ColumnCount() const
    {
        return m_columns.size();
    }

    bool IsEmpty() const
    {
        return m_rows.empty();
    }

private:
    std::vector<ColumnDefinition> m_columns;
    std::vector<Row> m_rows;
    bool m_truncate = true;
};

namespace details {

    // Splits text into word-boundary chunks of at most maxWidth chars.
    std::vector<std::wstring> WrapText(const std::wstring& text, size_t maxWidth);

} // namespace details

} // namespace wsl::windows::cli::table
