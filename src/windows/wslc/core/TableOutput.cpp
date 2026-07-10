/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TableOutput.cpp

Abstract:

    Non-templated implementation for TableOutput: FormattedCell rendering and
    the WrapText helper. The TableOutput<FieldCount> class template remains in
    the header.

--*/
#include "precomp.h"
#include "TableOutput.h"

using namespace wsl::windows::common::vt;

namespace wsl::windows::wslc {

FormattedCell::FormattedCell(std::wstring_view text, const Sequence& seq) : sequences({&seq, &Format::Default})
{
    WI_ASSERT(text.find(L"{}") == std::wstring_view::npos);
    fmt.reserve(4 + text.size());
    fmt += L"{}";
    fmt += text;
    fmt += L"{}";
}

size_t FormattedCell::VisibleWidth() const
{
    if (sequences.empty())
    {
        return fmt.size();
    }

    size_t width = 0;
    for (size_t i = 0; i < fmt.size(); ++i)
    {
        if (i + 1 < fmt.size() && fmt[i] == L'{' && fmt[i + 1] == L'}')
        {
            ++i; // skip the pair
        }
        else
        {
            ++width;
        }
    }
    return width;
}

std::wstring FormattedCell::Render(bool vtEnabled, bool colorEnabled) const
{
    if (sequences.empty())
    {
        return fmt; // plain text, no placeholders
    }

    std::wstring result;
    result.reserve(fmt.size() + (vtEnabled ? sequences.size() * 8 : 0));
    size_t seqIdx = 0;

    for (size_t i = 0; i < fmt.size(); ++i)
    {
        if (i + 1 < fmt.size() && fmt[i] == L'{' && fmt[i + 1] == L'}')
        {
            if (vtEnabled && seqIdx < sequences.size() && (colorEnabled || !sequences[seqIdx]->IsColor()))
            {
                result.append(sequences[seqIdx]->Get());
            }
            ++seqIdx;
            ++i; // skip the pair
        }
        else
        {
            result += fmt[i];
        }
    }

    return result;
}

std::wstring FormattedCell::RenderTruncated(size_t maxWidth, bool vtEnabled, bool colorEnabled) const
{
    if (sequences.empty())
    {
        // Plain text: simple truncation.
        if (fmt.size() <= maxWidth)
        {
            return fmt;
        }
        return fmt.substr(0, maxWidth > 0 ? maxWidth - 1 : 0) + L"\u2026";
    }

    std::wstring result;
    result.reserve(fmt.size());
    size_t seqIdx = 0;
    size_t visibleChars = 0;
    bool truncated = (maxWidth == 0);
    const size_t truncateAt = maxWidth > 1 ? maxWidth - 1 : 0;

    for (size_t i = 0; i < fmt.size(); ++i)
    {
        if (i + 1 < fmt.size() && fmt[i] == L'{' && fmt[i + 1] == L'}')
        {
            // Always emit sequences (they're invisible); they handle resets after truncation.
            if (vtEnabled && seqIdx < sequences.size() && (colorEnabled || !sequences[seqIdx]->IsColor()))
            {
                result.append(sequences[seqIdx]->Get());
            }
            ++seqIdx;
            ++i;
        }
        else if (!truncated)
        {
            if (visibleChars < truncateAt)
            {
                result += fmt[i];
                ++visibleChars;
            }
            else
            {
                result += L'\u2026';
                truncated = true;
            }
        }
        // After truncation, skip remaining visible chars but continue to emit sequences.
    }

    return result;
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

} // namespace wsl::windows::wslc
