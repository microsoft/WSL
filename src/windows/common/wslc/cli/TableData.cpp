/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableData.cpp

Abstract:

    Cell rendering, table construction helpers and the word-wrap helper for the
    CLI table data model.

--*/
#include "precomp.h"
#include "TableData.h"
#include "VTSupport.h"

using namespace wsl::windows::common::vt;

namespace wsl::windows::cli::table {

namespace {

    CellSequence Capture(const Sequence& sequence)
    {
        return CellSequence{std::wstring{sequence.Get()}, sequence.IsColor()};
    }

} // namespace

Cell::Cell(std::wstring_view text, const Sequence& style) : Text(text), Prefix(Capture(style)), Suffix(Capture(Format::Default))
{
}

Cell::Cell(std::wstring_view text, const Sequence& prefix, const Sequence& suffix) :
    Text(text), Prefix(Capture(prefix)), Suffix(Capture(suffix))
{
}

std::wstring Cell::Render(bool vtEnabled, bool colorEnabled) const
{
    return RenderTruncated(Text.size(), vtEnabled, colorEnabled);
}

std::wstring Cell::RenderTruncated(size_t maxWidth, bool vtEnabled, bool colorEnabled) const
{
    const auto emit = [&](const CellSequence& sequence) {
        return vtEnabled && !sequence.Empty() && (colorEnabled || !sequence.IsColor);
    };

    const bool withPrefix = emit(Prefix);
    const bool withSuffix = emit(Suffix);

    std::wstring_view text{Text};
    std::wstring_view ellipsis;
    if (text.size() > maxWidth)
    {
        text = text.substr(0, maxWidth > 0 ? maxWidth - 1 : 0);
        if (maxWidth > 0)
        {
            ellipsis = L"\u2026";
        }
    }

    if (!withPrefix && !withSuffix)
    {
        return std::wstring{text} + std::wstring{ellipsis};
    }

    std::wstring result;
    result.reserve(text.size() + 16);

    if (withPrefix)
    {
        result.append(Prefix.Text);
    }

    result.append(text);
    result.append(ellipsis);

    if (withSuffix)
    {
        result.append(Suffix.Text);
    }

    return result;
}

TableData::TableData(std::initializer_list<std::wstring_view> headers)
{
    m_columns.reserve(headers.size());
    for (const auto& header : headers)
    {
        m_columns.emplace_back(ColumnDefinition{std::wstring{header}, {}});
    }
}

TableData& TableData::AddColumn(std::wstring name, ColumnWidthConfig config)
{
    THROW_HR_IF(E_ILLEGAL_METHOD_CALL, !m_rows.empty());

    m_columns.emplace_back(ColumnDefinition{std::move(name), m_truncate ? config : ColumnWidthConfig{}});
    return *this;
}

void TableData::AddRow(std::vector<Cell> cells)
{
    AddRow(Row{std::move(cells), false});
}

void TableData::AddRow(Row row)
{
    THROW_HR_IF(E_INVALIDARG, row.Spanning || row.Cells.size() != m_columns.size());

    m_rows.emplace_back(std::move(row));
}

void TableData::AddSpanningRow(Cell cell)
{
    m_rows.emplace_back(Row{std::vector<Cell>{std::move(cell)}, true});
}

namespace details {

    std::vector<std::wstring> WrapText(const std::wstring& text, size_t maxWidth)
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

} // namespace details

} // namespace wsl::windows::cli::table
