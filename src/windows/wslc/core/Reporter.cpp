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

Reporter::Reporter() : m_out(GetStdHandle(STD_OUTPUT_HANDLE), stdout), m_err(GetStdHandle(STD_ERROR_HANDLE), stderr)
{
}

Reporter::Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled) :
    m_out(outFile, outVtEnabled), m_err(errFile, errVtEnabled)
{
}

std::wstring_view Reporter::LevelPrefix(Level level) const noexcept
{
    if (!IsColorEnabled(level))
    {
        return {};
    }

    switch (level)
    {
    case Level::Warning:
        return Format::Fg::BrightYellow.Get();
    case Level::Error:
        return Format::Fg::BrightRed.Get();
    default:
        return {};
    }
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

} // namespace wsl::windows::wslc
