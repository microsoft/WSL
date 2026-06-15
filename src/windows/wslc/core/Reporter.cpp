/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.cpp

Abstract:

    Implementation of Reporter.

--*/
#include "precomp.h"
#include "Reporter.h"

namespace wsl::windows::wslc {

using namespace wsl::windows::common::vt;

namespace {

    // Per-level SGR prefix. The reset (Format::Default) is always appended by
    // Emit() when VT is enabled, so each level only declares its colorant.
    const Sequence& LevelPrefix(Reporter::Level level)
    {
        switch (level)
        {
        case Reporter::Level::Debug:
            return Format::Dim;
        case Reporter::Level::Warning:
            return Format::Fg::BrightYellow;
        case Reporter::Level::Error:
            return Format::Fg::BrightRed;
        case Reporter::Level::Output:
        case Reporter::Level::Info:
        default:
            return Format::Default;
        }
    }

} // namespace

Reporter::Reporter() : m_out(GetStdHandle(STD_OUTPUT_HANDLE), stdout), m_err(GetStdHandle(STD_ERROR_HANDLE), stderr)
{
}

Reporter::Reporter(FILE* outFile) : m_out(outFile, false), m_err(outFile, false)
{
}

Reporter::Reporter(FILE* outFile, FILE* errFile) : m_out(outFile, false), m_err(errFile, false)
{
}

Reporter::Reporter(FILE* outFile, bool vtEnabled) : m_out(outFile, vtEnabled), m_err(outFile, vtEnabled)
{
}

Reporter::Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled) :
    m_out(outFile, outVtEnabled), m_err(errFile, errVtEnabled)
{
}

void Reporter::Emit(const OutputChannel& channel, Level level, std::wstring_view body, bool appendNewline) const
{
    const bool vtEnabled = channel.IsVTEnabled();
    const bool colorEnabled = vtEnabled && !m_noColor;

    // Avoid an allocation when there is nothing to wrap: VT off, no newline,
    // and the body is already a complete string. WriteString is a no-op for
    // empty input, so the empty-body path also short-circuits here.
    if (!vtEnabled && !appendNewline)
    {
        channel.WriteString(body);
        return;
    }

    std::wstring out;
    const std::wstring_view prefix = colorEnabled ? LevelPrefix(level).Get() : std::wstring_view{};
    const std::wstring_view reset = colorEnabled ? Format::Default.Get() : std::wstring_view{};

    out.reserve(prefix.size() + body.size() + reset.size() + (appendNewline ? 1 : 0));
    out.append(prefix);
    out.append(body);
    out.append(reset);
    if (appendNewline)
    {
        out.push_back(L'\n');
    }

    channel.WriteString(out);
}

bool Reporter::IsVTEnabled(Level level) const noexcept
{
    return ChannelFor(level).IsVTEnabled();
}

bool Reporter::IsColorEnabled(Level level) const noexcept
{
    return ChannelFor(level).IsVTEnabled() && !m_noColor;
}

std::optional<int> Reporter::GetConsoleWidth(Level level) const
{
    return ChannelFor(level).GetConsoleWidth();
}

bool Reporter::IsLevelEnabled(Level level) const noexcept
{
    return WI_AreAllFlagsSet(m_enabledLevels, level);
}

void Reporter::SetLevelMask(Level level, bool enabled) noexcept
{
    if (enabled)
    {
        WI_SetAllFlags(m_enabledLevels, level);
    }
    else
    {
        WI_ClearAllFlags(m_enabledLevels, level);
    }
}

void Reporter::Disable() noexcept
{
    m_enabled.store(false, std::memory_order_relaxed);
}

} // namespace wsl::windows::wslc
