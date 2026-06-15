/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageProgressCallback.cpp

Abstract:

    This file contains the ImageProgressCallback Implementation.

--*/

#include "precomp.h"
#include "ImageProgressCallback.h"
#include "ImageService.h"
#include <format>

namespace wsl::windows::wslc::services {
using namespace wsl::shared;
using namespace wsl::windows::common::vt;

namespace {
    // The docker engine emits terminal pull/push results as global progress events
    // whose `status` begins with "Status: " (e.g. "Status: Downloaded newer image for
    // alpine:latest" or "Status: Image is up to date for alpine:latest"). The CLI
    // forwards these verbatim to stdout so scripts can capture the authoritative
    // outcome with `wslc image pull ... > result.txt`. All other progress chatter
    // stays on stderr.
    constexpr std::string_view c_finalStatusPrefix = "Status: ";

    bool IsFinalStatus(LPCSTR status)
    {
        return status != nullptr && std::string_view{status}.substr(0, c_finalStatusPrefix.size()) == c_finalStatusPrefix;
    }
} // namespace

void ImageProgressCallback::WriteTerminal(std::wstring_view content) const
{
    // Route through the Reporter's Info channel (stderr) so progress chatter doesn't
    // pollute stdout for scripting, while preserving atomic per-call flushing.
    m_output.Info() << content << std::flush;
}

auto ImageProgressCallback::MoveToLine(int line)
{
    if (line > 0)
    {
        WriteTerminal(Cursor::Up(line).Get());
    }

    return wil::scope_exit([line = line, this]() {
        if (line > 1)
        {
            WriteTerminal(Cursor::Down(line - 1).Get());
        }
    });
}

HRESULT ImageProgressCallback::OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total)
{
    try
    {
        if (!m_vtEnabled)
        {
            // Plain (redirected) mode: emit Docker-style one-line transitions.
            // For per-layer events, only print when the status text changes (e.g.
            // "Pulling fs layer" -> "Downloading" -> "Download complete") so we
            // don't spam the log with every byte update.
            if (status == nullptr || *status == '\0')
            {
                return S_OK;
            }

            if (id == nullptr || *id == '\0')
            {
                // Final "Status: ..." lines from the daemon go to stdout; all other
                // global progress text stays on stderr.
                if (IsFinalStatus(status))
                {
                    // Clear the progress line so the final status doesn't get mixed in with it. This
                    // is especially important when output is redirected, since the final status is
                    // the only thing that goes to stdout and we don't want it to be preceded by
                    // a bunch of backspace characters.
                    WriteTerminal(Erase::LineEntirely.Get());
                    m_output.Output() << wsl::shared::string::MultiByteToWide(status) << std::endl;
                }
                else
                {
                    WriteTerminal(std::format(L"{}\n", status));
                }
            }
            else
            {
                std::string statusStr{status};
                auto& last = m_plainStatuses[id];
                if (last != statusStr)
                {
                    last = statusStr;
                    m_output.Info() << wsl::shared::string::MultiByteToWide(id) << L": "
                                    << wsl::shared::string::MultiByteToWide(statusStr) << std::endl;
                }
            }

            return S_OK;
        }

        if (id == nullptr || *id == '\0') // Print all 'global' statuses on their own line
        {
            if (IsFinalStatus(status))
            {
                // Final outcome from the daemon: route to stdout so callers can capture
                // the authoritative "Status: Downloaded ..." / "Status: Image is up to
                // date ..." line without parsing progress chatter.
                m_output.Output() << wsl::shared::string::MultiByteToWide(status) << std::endl;
            }
            else
            {
                WriteTerminal(std::format(L"{}\n", status));
            }
            m_currentLine++;
            return S_OK;
        }

        const auto visibleWidth = m_output.GetConsoleWidth(Reporter::Level::Info);

        auto it = m_statuses.find(id);
        if (it == m_statuses.end())
        {
            // If this is the first time we see this ID, create a new line for it.
            m_statuses.emplace(id, m_currentLine);
            WriteTerminal(GenerateStatusLine(status, id, current, total, visibleWidth) + L'\n');
            m_currentLine++;
        }
        else
        {
            auto revert = MoveToLine(m_currentLine - it->second);
            WriteTerminal(GenerateStatusLine(status, id, current, total, visibleWidth) + L'\n');
        }

        return S_OK;
    }
    CATCH_RETURN();
}

std::wstring ImageProgressCallback::GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, std::optional<int> visibleWidth)
{
    std::wstring line;
    if (total != 0)
    {
        constexpr int c_progressBarWidth = 30;

        int filled = 0;
        if (current >= total)
        {
            filled = c_progressBarWidth;
        }
        else
        {
            auto ratio = static_cast<long double>(current) / static_cast<long double>(total);
            filled = static_cast<int>(ratio * c_progressBarWidth);
        }

        filled = std::clamp(filled, 0, c_progressBarWidth);

        std::wstring bar;
        bar.reserve(c_progressBarWidth);
        bar.append(filled, L'=');
        bar.append(L">");
        bar.resize(c_progressBarWidth, L' ');

        // Docker's reported total is an estimate of the compressed layer size, so the actual bytes
        // transferred can exceed it. Drop the total in that case to avoid displaying a count over 100%.
        auto progress = wsl::shared::string::FormatBytes(current);

        if (current <= total)
        {
            progress += std::format(L"/{}", wsl::shared::string::FormatBytes(total));
        }

        line = std::format(L"{}: {} [{}] {}", id, status, bar, progress);
    }
    else if (current != 0)
    {
        line = std::format(L"{}: {} {}", id, status, wsl::shared::string::FormatBytes(current));
    }
    else
    {
        line = std::format(L"{}: {}", id, status);
    }

    // Truncate to console width to prevent wrapping that would break cursor repositioning.
    // When visibleWidth is unknown (redirected) write the line as-is.
    if (visibleWidth)
    {
        if (line.size() > static_cast<size_t>(*visibleWidth))
        {
            line.resize(*visibleWidth);

            // Avoid splitting a surrogate pair — if the last code unit is a high surrogate,
            // drop it so we don't emit an invalid UTF-16 sequence.
            if (!line.empty() && IS_HIGH_SURROGATE(line.back()))
            {
                line.pop_back();
            }
        }

        // Erase any previously written char on that line.
        line.resize(*visibleWidth, L' ');
    }

    return line;
}
} // namespace wsl::windows::wslc::services
