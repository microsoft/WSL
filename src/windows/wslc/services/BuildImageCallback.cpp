/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildImageCallback.cpp

Abstract:

    This file contains the BuildImageCallback Implementation.

--*/

#include "precomp.h"
#include "BuildImageCallback.h"
#include <docker_schema.h>

namespace wsl::windows::wslc::services {

using wsl::windows::common::string::MultiByteToWide;

// ── Formatting helpers ───────────────────────────────────────────────

static std::wstring FormatBytes(int64_t b)
{
    if (b <= 0)
    {
        return L"0B";
    }

    constexpr double c_kB = 1024.0;
    constexpr double c_MB = 1024.0 * 1024.0;
    constexpr double c_GB = 1024.0 * 1024.0 * 1024.0;
    auto val = static_cast<double>(b);

    if (val >= c_GB)
    {
        return std::format(L"{:.2f}GB", val / c_GB);
    }
    if (val >= c_MB)
    {
        return std::format(L"{:.2f}MB", val / c_MB);
    }
    if (val >= c_kB)
    {
        return std::format(L"{:.2f}kB", val / c_kB);
    }
    return std::format(L"{}B", b);
}

static std::wstring FormatDuration(std::chrono::milliseconds d)
{
    auto totalSeconds = d.count() / 1000;

    if (totalSeconds >= 60)
    {
        auto minutes = totalSeconds / 60;
        auto seconds = totalSeconds % 60;
        return std::format(L"{}m{:.0f}s", minutes, static_cast<double>(seconds));
    }

    return std::format(L"{:.1f}s", d.count() / 1000.0);
}

static std::wstring FormatElapsed(std::chrono::steady_clock::time_point start)
{
    return FormatDuration(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start));
}

// ── BuildImageCallback ──────────────────────────────────────────────

BuildImageCallback::~BuildImageCallback()
try
{
    StopTimerThread();

    if (m_isConsole)
    {
        bool hasError = !IsCancelled() && std::uncaught_exceptions() > m_uncaughtExceptions;

        CONSOLE_SCREEN_BUFFER_INFO info{};
        THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(m_console, &info));
        const SHORT consoleWidth = std::max<SHORT>(40, info.srWindow.Right - info.srWindow.Left);

        // Erase the live display area.
        std::wstring eraseCmd;
        if (m_linesWrittenLastFrame >= m_viewportHeight)
        {
            // Fixed mode: display fills the viewport, go to top.
            eraseCmd = L"\033[H\033[J";
        }
        else if (m_linesWrittenLastFrame > 0)
        {
            // Append mode: go back to frame start and erase.
            eraseCmd = std::format(L"\033[{}A\r\033[J", m_linesWrittenLastFrame);
        }

        if (!eraseCmd.empty())
        {
            WriteTerminal(eraseCmd);
        }

        // Print the full final render permanently. All lines are written so
        // the user can scroll back and see the complete build output.
        {
            std::lock_guard lock(m_mutex);
            auto allLines = BuildFrameLines(consoleWidth);
            std::wstring buffer;
            for (const auto& line : allLines)
            {
                buffer += line;
                buffer += L"\n";
            }

            // Append error details for failing steps.
            if (hasError)
            {
                for (const auto& target : m_buildView.GetTargets())
                {
                    for (const auto& step : target.steps)
                    {
                        if (!step.error.empty())
                        {
                            buffer += std::format(L"\n\033[31m Error: {}\033[0m\n", MultiByteToWide(step.error));
                            if (!step.logOutput.empty())
                            {
                                buffer += L"\033[2m";
                                for (const auto& logLine : step.logOutput)
                                {
                                    buffer += L"  ";
                                    buffer += MultiByteToWide(logLine);
                                    buffer += L"\n";
                                }
                                buffer += L"\033[0m";
                            }
                        }
                    }
                }
            }

            WriteTerminal(buffer);
        }

        WriteTerminal(L"\033[?25h");
    }
}
CATCH_LOG()

void BuildImageCallback::WriteTerminal(std::wstring_view content) const
{
    DWORD written;
    LOG_IF_WIN32_BOOL_FALSE(WriteConsoleW(m_console, content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
}

bool BuildImageCallback::IsCancelled() const
{
    return WaitForSingleObject(m_cancelEvent, 0) == WAIT_OBJECT_0;
}

void BuildImageCallback::StartTimerThread()
{
    if (!m_isConsole || m_timerThread.joinable())
    {
        return;
    }

    m_stopEvent.create(wil::EventOptions::ManualReset);
    m_timerThread = std::thread([this]() {
        while (WaitForSingleObject(m_stopEvent.get(), 200) == WAIT_TIMEOUT)
        {
            try
            {
                std::lock_guard lock(m_mutex);
                RenderFrame();
            }
            CATCH_LOG()
        }
    });
}

void BuildImageCallback::StopTimerThread()
{
    if (m_timerThread.joinable())
    {
        m_stopEvent.SetEvent();
        m_timerThread.join();
    }
}

HRESULT BuildImageCallback::OnProgress(LPCSTR status, LPCSTR id, ULONGLONG /*current*/, ULONGLONG /*total*/)
try
{
    if (status == nullptr || *status == '\0' || IsCancelled())
    {
        return S_OK;
    }

    std::string statusStr(status);
    std::string idStr(id ? id : "");

    // BuildKit JSON messages are identified by the "buildkit" id
    if (idStr == "buildkit")
    {
        StartTimerThread();

        std::lock_guard lock(m_mutex);

        try
        {
            auto json = nlohmann::json::parse(statusStr);
            wsl::windows::common::docker_schema::BuildKitSolveStatus solveStatus{};
            from_json(json, solveStatus);
            m_buildView.ProcessMessage(solveStatus);
        }
        catch (...)
        {
            // Ignore malformed JSON
        }
    }

    return S_OK;
}
CATCH_RETURN();

// ── Step line formatter ─────────────────────────────────────────────

std::wstring BuildImageCallback::FormatStepLine(const ViewStep& step, SHORT consoleWidth) const
{
    auto wtext = MultiByteToWide(step.stepLabel);

    // Truncate sha256 hashes for readability (keep prefix + 12 hex chars).
    auto shaPos = wtext.find(L"@sha256:");
    if (shaPos != std::wstring::npos && shaPos + 20 < wtext.size())
    {
        wtext.resize(shaPos + 20);
    }

    // Right-aligned status indicator.
    std::wstring timer;
    if (step.cached)
    {
        timer = L"CACHED";
    }
    else if (step.completed)
    {
        timer = FormatDuration(step.elapsed);
    }
    else if (step.startTime != std::chrono::steady_clock::time_point{})
    {
        timer = FormatElapsed(step.startTime);
    }

    // Layout: step text + padding + timer, truncating the step text if needed.
    auto timerWidth = static_cast<SHORT>(timer.size());
    auto maxTextWidth = static_cast<size_t>(std::max<SHORT>(10, consoleWidth - timerWidth - 2));

    if (wtext.size() > maxTextWidth && maxTextWidth > 3)
    {
        wtext.resize(maxTextWidth - 3);
        wtext += L"...";
    }

    auto usedWidth = wtext.size() + static_cast<size_t>(timerWidth);
    auto paddingSize = (usedWidth < static_cast<size_t>(consoleWidth)) ? static_cast<size_t>(consoleWidth) - usedWidth : 0;
    std::wstring padding(std::max<size_t>(1, paddingSize), L' ');

    // Cyan for completed/cached, bold for running, red for errors.
    if (!step.error.empty())
    {
        return std::format(L"\033[31m{}{}{}\033[0m", wtext, padding, timer);
    }
    else if (step.completed || step.cached)
    {
        return std::format(L"\033[36m{}{}{}\033[0m", wtext, padding, timer);
    }
    else
    {
        return std::format(L"\033[1m{}{}{}\033[0m", wtext, padding, timer);
    }
}

// ── Frame line builder ──────────────────────────────────────────────
// Builds all frame lines into a vector. Each entry is one visual line
// (no embedded newlines). Used by both RenderFrame and the destructor's
// final permanent output.

std::vector<std::wstring> BuildImageCallback::BuildFrameLines(SHORT consoleWidth) const
{
    std::vector<std::wstring> lines;

    // Total build timer header (right-aligned, dimmed)
    {
        auto elapsed = FormatElapsed(m_buildStartTime);
        auto label = std::format(L"Building {}", elapsed);
        auto pad = static_cast<size_t>(std::max<SHORT>(0, consoleWidth - static_cast<SHORT>(label.size())));
        lines.push_back(std::format(L"{}\033[2m{}\033[0m", std::wstring(pad, L' '), label));
    }

    bool firstTarget = true;

    for (const auto& target : m_buildView.GetTargets())
    {
        bool hasVisibleSteps = false;
        for (const auto& step : target.steps)
        {
            if (step.started || step.completed)
            {
                hasVisibleSteps = true;
                break;
            }
        }

        if (!hasVisibleSteps)
        {
            continue;
        }

        // Blank line separator between targets (not before first).
        if (!firstTarget)
        {
            lines.emplace_back();
        }
        firstTarget = false;

        // Synthesize cached FROM steps for shared base images
        if (!target.steps.empty())
        {
            const auto& firstStep = target.steps[0];
            if (firstStep.stepNum > 1 && firstStep.stepTotal > 0)
            {
                for (uint32_t n = 1; n < firstStep.stepNum; n++)
                {
                    std::string fromImage = " ...";
                    for (const auto& digest : firstStep.inputs)
                    {
                        auto* inputStep = m_buildView.StepByDigest(digest);
                        if (inputStep && inputStep->stepLabel.find("] FROM ") != std::string::npos)
                        {
                            auto pos = inputStep->stepLabel.find("] FROM ");
                            if (pos != std::string::npos)
                            {
                                fromImage = inputStep->stepLabel.substr(pos + 6);
                            }
                            break;
                        }
                    }
                    auto label = std::format("[{} {}/{}] FROM{}", target.name, n, firstStep.stepTotal, fromImage);
                    auto wlabel = MultiByteToWide(label);
                    if (static_cast<SHORT>(wlabel.size()) > consoleWidth - 10)
                    {
                        wlabel.resize(consoleWidth - 13);
                        wlabel += L"...";
                    }
                    auto padLen = (wlabel.size() + 6 < static_cast<size_t>(consoleWidth))
                                      ? static_cast<size_t>(consoleWidth) - wlabel.size() - 6
                                      : 0;
                    lines.push_back(
                        std::format(L"\033[36m{}{}{}\033[0m", wlabel, std::wstring(std::max<size_t>(1, padLen), L' '), L"CACHED"));
                }
            }
        }

        // Steps
        for (const auto& step : target.steps)
        {
            if (!step.started && !step.completed)
            {
                continue;
            }

            lines.push_back(FormatStepLine(step, consoleWidth));

            // Below a step: show layer progress bars OR recent log output.
            bool showLayers = false;
            if (!step.completed && !step.subStatuses.empty())
            {
                showLayers =
                    std::any_of(step.subStatuses.begin(), step.subStatuses.end(), [](const auto& p) { return !p.second.completed; });
            }

            if (showLayers)
            {
                for (const auto& [_, sub] : step.subStatuses)
                {
                    auto sid = MultiByteToWide(ShortDigest(sub.id));

                    if (sub.completed && sub.total > 0)
                    {
                        lines.push_back(std::format(L"\033[2m  {}: done {}\033[0m", sid, FormatBytes(sub.total)));
                    }
                    else if (sub.completed)
                    {
                        lines.push_back(std::format(L"\033[2m  {}: done\033[0m", sid));
                    }
                    else if (sub.total > 0)
                    {
                        constexpr int c_barWidth = 30;
                        auto ratio = static_cast<double>(sub.current) / static_cast<double>(sub.total);
                        auto filled = std::clamp(static_cast<int>(ratio * c_barWidth), 0, c_barWidth);
                        auto emptyWidth = c_barWidth - filled;

                        std::wstring filledBar(filled, L'=');
                        std::wstring emptyBar(emptyWidth, L' ');
                        if (filled < c_barWidth && !emptyBar.empty())
                        {
                            emptyBar[0] = L'>';
                        }

                        lines.push_back(std::format(
                            L"\033[2m  {}: [\033[32m{}\033[0m\033[2m{}] {}/{}\033[0m", sid, filledBar, emptyBar, FormatBytes(sub.current), FormatBytes(sub.total)));
                    }
                    else
                    {
                        lines.push_back(std::format(L"\033[2m  {}: waiting...\033[0m", sid));
                    }
                }
            }
            else if (!step.completed && !step.logOutput.empty())
            {
                auto tailCount = std::min(step.logOutput.size(), c_maxLogPeekLines);
                auto tailStart = step.logOutput.size() - tailCount;
                for (size_t i = tailStart; i < step.logOutput.size(); i++)
                {
                    auto wline = MultiByteToWide(step.logOutput[i]);
                    if (static_cast<SHORT>(wline.size()) > consoleWidth - 4)
                    {
                        wline.resize(consoleWidth - 4);
                    }
                    lines.push_back(std::format(L"\033[2m  {}\033[0m", wline));
                }
            }

            // Error message
            if (!step.error.empty())
            {
                auto errorW = MultiByteToWide(step.error);
                if (static_cast<SHORT>(errorW.size()) > consoleWidth - 4)
                {
                    errorW.resize(consoleWidth - 7);
                    errorW += L"...";
                }
                lines.push_back(std::format(L"\033[31m  {}\033[0m", errorW));
            }
        }
    }

    return lines;
}

// ── Frame renderer ──────────────────────────────────────────────────
//   Renders in two modes. First it appends and then once it hits the
//   max viewport width it goes to fixed mode.
//
//
//   Append mode (totalLines < viewportHeight):
//     Content fits in the viewport. Use cursor-up to overwrite previous
//     frame in-place. Safe because cursor-up stays within the viewport.
//
//   Fixed mode (totalLines >= viewportHeight):
//     Content exceeds viewport. Use \033[H (cursor home) to go to the
//     top of the viewport. Write header + tail, filling exactly the
//     viewport. Last line has no trailing newline to prevent scrolling.

void BuildImageCallback::RenderFrame()
{
    CONSOLE_SCREEN_BUFFER_INFO info{};
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(m_console, &info));
    const SHORT consoleWidth = std::max<SHORT>(40, info.srWindow.Right - info.srWindow.Left);

    // Snapshot viewport height on first render.
    if (m_viewportHeight == 0)
    {
        m_viewportHeight = info.srWindow.Bottom - info.srWindow.Top + 1;
    }

    auto allLines = BuildFrameLines(consoleWidth);
    auto totalLines = static_cast<SHORT>(allLines.size());

    std::wstring buffer = L"\033[?25l"; // Hide cursor during redraw.

    if (totalLines < m_viewportHeight)
    {
        // Append mode: content fits in viewport.
        if (m_linesWrittenLastFrame > 0)
        {
            buffer += std::format(L"\033[{}A\r", m_linesWrittenLastFrame);
        }

        for (const auto& line : allLines)
        {
            buffer += line;
            buffer += L"\033[K\n";
        }
        buffer += L"\033[J";

        m_linesWrittenLastFrame = totalLines;
    }
    else
    {
        // Fixed mode: fill the entire viewport.
        if (m_linesWrittenLastFrame >= m_viewportHeight)
        {
            // Already in fixed mode: cursor home to viewport top.
            buffer += L"\033[H";
        }
        else if (m_linesWrittenLastFrame > 0)
        {
            // Transition from append mode: go back to start of old frame.
            buffer += std::format(L"\033[{}A\r", m_linesWrittenLastFrame);
        }

        // Header line
        buffer += allLines[0];
        buffer += L"\033[K\n";

        // Tail: fill the rest of the viewport with the most recent content.
        auto tailCount = static_cast<size_t>(m_viewportHeight - 1);
        auto tailStart = allLines.size() - tailCount;
        for (size_t i = tailStart; i < allLines.size(); i++)
        {
            buffer += allLines[i];
            buffer += L"\033[K";
            if (i + 1 < allLines.size())
            {
                buffer += L"\n";
            }
            // Last line: no \n to prevent viewport scrolling.
        }

        m_linesWrittenLastFrame = m_viewportHeight;
    }

    WriteTerminal(buffer);
}

} // namespace wsl::windows::wslc::services
