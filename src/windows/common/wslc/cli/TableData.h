/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableData.h

Abstract:

    Data model for tabular CLI output. A table is a list of column definitions
    plus a list of rows; a row holds one cell per column, or a single spanning
    cell that occupies the whole line. Cells carry their own text and optional
    VT sequences, which are zero display width, so a cell's visible width is
    simply its text length.

    A table holds every row it emits: column widths are measured across the complete
    set so that each row aligns against widths that fit it. Callers therefore build a
    table from a result set they already hold in full, and should call Reserve() with
    the expected row count so row storage is allocated once.

    This header is rendering-agnostic and does not depend on Terminal; see
    TableRenderer.h for layout and emission.

--*/
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "VTSupport.h"

namespace wsl::windows::wslc::cli {

using wsl::windows::common::vt::Sequence;

// A single table cell. Prefix and Suffix are emitted around Text when the destination supports
// VT; they contribute no display width. The pair covers both styling (color + reset) and
// paired constructs such as hyperlink open/close.
struct Cell
{
    std::wstring Text;
    const Sequence* Prefix = nullptr;
    const Sequence* Suffix = nullptr;

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

    // Styled cell: wraps the text with the sequence and a trailing reset.
    Cell(std::wstring_view text, const Sequence& style);

    // Paired cell: wraps the text with an explicit open and close sequence.
    Cell(std::wstring_view text, const Sequence& prefix, const Sequence& suffix);

    // Block temporaries: the cell only stores pointers, so binding a Sequence rvalue (including
    // derived types such as the ConstructedSequence returned by Sgr()) would dangle once the full
    // expression ends. Only long-lived Sequence instances may be used here.
    Cell(std::wstring_view text, const Sequence&& style) = delete;
    Cell(std::wstring_view text, const Sequence&& prefix, const Sequence&& suffix) = delete;

    size_t VisibleWidth() const
    {
        return Text.size();
    }

    // Renders the cell with or without its sequences.
    // When vtEnabled is false, no sequences are emitted.
    // When vtEnabled is true but colorEnabled is false, only non-color sequences are emitted.
    std::wstring Render(bool vtEnabled, bool colorEnabled) const;

    // Renders with text truncated to maxWidth characters, appending an ellipsis.
    // Sequences are still emitted so styling is closed correctly.
    std::wstring RenderTruncated(size_t maxWidth, bool vtEnabled, bool colorEnabled) const;
};

// A table row. A normal row holds one cell per column. A spanning row holds a single cell that is
// emitted as a standalone line: it takes no part in column sizing and receives no indent, which
// suits section headings and blank separators.
struct Row
{
    std::vector<Cell> Cells;
    bool Spanning = false;
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

// Spacing inserted between columns.
inline constexpr size_t c_defaultColumnPadding = 3;

// Minimum total width of a column including its padding. Combined with the default padding this
// gives list output the same column rhythm as other container CLIs. The final column is exempt:
// it emits no trailing padding, so a minimum there would only add trailing whitespace.
inline constexpr size_t c_defaultMinCellWidth = 10;

// Table contents and presentation options. Build one of these with columns, add rows, then hand it
// to RenderTable().
struct TableData
{
    std::vector<ColumnDefinition> Columns;
    std::vector<Row> Rows;

    // Emits the column names as a leading row. When false the header is omitted even if the table
    // has no rows.
    bool ShowHeader = true;

    // Spaces prepended to every non-spanning row. Does not affect column width calculations.
    size_t RowIndent = 0;

    size_t ColumnPadding = c_defaultColumnPadding;
    size_t MinCellWidth = c_defaultMinCellWidth;

    // Overrides console width for column fitting; 0 uses the Terminal-derived width. When set, the
    // wrap pass runs as if a real console were attached.
    size_t ConsoleWidthOverride = 0;

    TableData() = default;

    explicit TableData(std::vector<ColumnDefinition> columns) : Columns(std::move(columns))
    {
    }

    // Convenience for the common case of headers without per-column width configuration.
    TableData(std::initializer_list<std::wstring_view> headers);

    // Allocates row storage for the expected number of rows.
    void Reserve(size_t rowCount)
    {
        Rows.reserve(rowCount);
    }

    // Appends a data row. The cell count must match the column count.
    void AddRow(std::vector<Cell> cells);

    // Appends a standalone line that does not participate in column sizing.
    void AddLine(Cell cell = {});

    size_t ColumnCount() const
    {
        return Columns.size();
    }

    bool IsEmpty() const
    {
        return Rows.empty();
    }
};

namespace details {

    // Splits text into word-boundary chunks of at most maxWidth chars.
    std::vector<std::wstring> WrapText(const std::wstring& text, size_t maxWidth);

} // namespace details

} // namespace wsl::windows::wslc::cli
