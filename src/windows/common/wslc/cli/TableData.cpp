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

using namespace wsl::windows::common::vt;

namespace wsl::windows::wslc::cli {

Cell::Cell(std::wstring_view text, const Sequence& style) : Text(text), Prefix(&style), Suffix(&Format::Default)
{
}

Cell::Cell(std::wstring_view text, const Sequence& prefix, const Sequence& suffix) : Text(text), Prefix(&prefix), Suffix(&suffix)
{
}

std::wstring Cell::Render(bool vtEnabled, bool colorEnabled) const
{
    return RenderTruncated(Text.size(), vtEnabled, colorEnabled);
}

std::wstring Cell::RenderTruncated(size_t maxWidth, bool vtEnabled, bool colorEnabled) const
{
    const auto emit = [&](const Sequence* sequence) { return vtEnabled && sequence && (colorEnabled || !sequence->IsColor()); };

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
        result.append(Prefix->Get());
    }

    result.append(text);
    result.append(ellipsis);

    if (withSuffix)
    {
        result.append(Suffix->Get());
    }

    return result;
}

TableData::TableData(std::initializer_list<std::wstring_view> headers)
{
    Columns.reserve(headers.size());
    for (const auto& header : headers)
    {
        Columns.emplace_back(ColumnDefinition{std::wstring{header}, {}});
    }
}

void TableData::AddRow(std::vector<Cell> cells)
{
    THROW_HR_IF(E_INVALIDARG, cells.size() != Columns.size());

    Rows.emplace_back(Row{std::move(cells), false});
}

void TableData::AddLine(Cell cell)
{
    Rows.emplace_back(Row{std::vector<Cell>{std::move(cell)}, true});
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

} // namespace wsl::windows::wslc::cli
