/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableRenderer.h

Abstract:

    Layout and emission for the CLI table data model. Rendering runs in three
    passes over a fully populated table: measure each column against its rows,
    fit the columns to the available console width, then emit.

--*/
#pragma once

#include "TableData.h"
#include "Terminal.h"

namespace wsl::windows::wslc::cli {

// Generous fallback used when the destination is redirected (no real console width).
// The wrap pass is skipped in that case so the receiver controls its own width.
inline constexpr size_t c_redirectedConsoleWidth = 2000;

// Lays out and writes the table to the terminal.
void RenderTable(Terminal& terminal, const TableData& table, Terminal::Level level = Terminal::Level::Output);

} // namespace wsl::windows::wslc::cli
