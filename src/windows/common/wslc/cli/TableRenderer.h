/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableRenderer.h

Abstract:

    Layout and emission for the CLI table data model. Rendering runs in three
    passes over a fully populated table: measure each column against its rows,
    fit the columns to the available console width, then emit.

    Renderers share the layout and differ only in the options they supply and
    in how the resulting lines reach the terminal.

--*/
#pragma once

#include <optional>

#include "TableData.h"
#include "Terminal.h"

namespace wsl::windows::cli::table {

using wsl::windows::wslc::cli::Terminal;

// Fallback used when the destination is redirected. The wrap pass is skipped in that case so the
// receiver controls its own width.
inline constexpr size_t c_redirectedConsoleWidth = 2000;

struct LayoutOptions
{
    // Nullopt when the destination is redirected.
    std::optional<size_t> ConsoleWidth;
    bool VtEnabled = false;
    bool ColorEnabled = false;
};

struct TableLayout
{
    std::vector<size_t> ColumnWidths;

    // Fully formatted physical lines, without line terminators.
    std::vector<std::wstring> Lines;
};

// Render() lays the table out and hands the result to Emit(); derived classes customize the layout
// options and the emission, but not the layout itself.
class TableRenderer
{
public:
    // consoleWidth replaces the width reported by the terminal.
    TableRenderer(Terminal& terminal, Terminal::Level level, std::optional<size_t> consoleWidth = std::nullopt) :
        m_terminal(terminal), m_level(level), m_consoleWidth(consoleWidth)
    {
    }

    virtual ~TableRenderer() = default;

    TableRenderer(const TableRenderer&) = delete;
    TableRenderer& operator=(const TableRenderer&) = delete;

    void Render(const TableData& table);

    // Measures and fits the table's columns and formats every line it emits.
    static TableLayout Layout(const TableData& table, const LayoutOptions& options);

protected:
    virtual LayoutOptions GetLayoutOptions() const;

    virtual void Emit(const TableLayout& layout) = 0;

    Terminal& m_terminal;
    Terminal::Level m_level;
    std::optional<size_t> m_consoleWidth;
};

// Writes each line once, in order.
class StaticTableRenderer final : public TableRenderer
{
public:
    using TableRenderer::TableRenderer;

protected:
    void Emit(const TableLayout& layout) override;
};

// Lays out and writes the table to the terminal with a StaticTableRenderer.
void RenderTable(Terminal& terminal, const TableData& table, Terminal::Level level = Terminal::Level::Output, std::optional<size_t> consoleWidth = std::nullopt);

} // namespace wsl::windows::cli::table
