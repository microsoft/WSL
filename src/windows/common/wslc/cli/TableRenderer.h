/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableRenderer.h

Abstract:

    Layout and emission for the CLI table data model. Rendering runs in three
    passes over a fully populated table: measure each column against its rows,
    fit the columns to the available console width, then emit.

    Layout is shared by every renderer and has no terminal dependency. Renderers
    differ only in the layout options they supply and in how the resulting lines
    reach the terminal.

--*/
#pragma once

#include <optional>

#include "TableData.h"
#include "Terminal.h"

namespace wsl::windows::cli::table {

using wsl::windows::wslc::cli::Terminal;

// Generous fallback used when the destination is redirected (no real console width).
// The wrap pass is skipped in that case so the receiver controls its own width.
inline constexpr size_t c_redirectedConsoleWidth = 2000;

struct LayoutOptions
{
    // Width of the attached console; nullopt when the destination is redirected.
    // TableData::ConsoleWidthOverride takes precedence when set.
    std::optional<size_t> ConsoleWidth;
    bool VtEnabled = false;
    bool ColorEnabled = false;
};

struct TableLayout
{
    // Final visible width of each column, in column order.
    std::vector<size_t> ColumnWidths;

    // Fully formatted physical lines, without line terminators.
    std::vector<std::wstring> Lines;
};

// Base class for table renderers. Render() lays the table out and hands the result to Emit();
// derived classes customize the layout options and the emission, but not the layout itself.
class TableRenderer
{
public:
    TableRenderer(Terminal& terminal, Terminal::Level level) : m_terminal(terminal), m_level(level)
    {
    }

    virtual ~TableRenderer() = default;

    TableRenderer(const TableRenderer&) = delete;
    TableRenderer& operator=(const TableRenderer&) = delete;

    void Render(const TableData& table);

    // Measures and fits the table's columns and formats every line it emits.
    static TableLayout Layout(const TableData& table, const LayoutOptions& options);

protected:
    // Derives the console width and VT/color support from the terminal.
    virtual LayoutOptions GetLayoutOptions() const;

    virtual void Emit(const TableLayout& layout) = 0;

    Terminal& m_terminal;
    Terminal::Level m_level;
};

// Writes each line once, in order. Suitable for any destination, including redirected output.
class StaticTableRenderer final : public TableRenderer
{
public:
    using TableRenderer::TableRenderer;

protected:
    void Emit(const TableLayout& layout) override;
};

// Lays out and writes the table to the terminal with a StaticTableRenderer.
void RenderTable(Terminal& terminal, const TableData& table, Terminal::Level level = Terminal::Level::Output);

} // namespace wsl::windows::cli::table
