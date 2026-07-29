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

Reporter::Reporter() :
    m_out(GetStdHandle(STD_OUTPUT_HANDLE), stdout), m_err(GetStdHandle(STD_ERROR_HANDLE), stderr), m_in(GetStdHandle(STD_INPUT_HANDLE), stdin)
{
}

Reporter::Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled, FILE* inFile, bool inInteractive) :
    m_out(outFile, outVtEnabled), m_err(errFile, errVtEnabled), m_in(inFile, inInteractive)
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

std::wstring Reporter::PromptForLine(Level level, std::wstring_view label, bool mask)
{
    // Write the label without a trailing newline so the cursor stays inline (matching
    // Docker's prompt behavior).
    Write(level, L"{}", label);

    const bool willMask = mask && m_in.IsInteractive();
    auto line = m_in.ReadLine(mask);

    // When echo was masked the user's Enter was not echoed, so advance the line here.
    if (willMask)
    {
        Write(level, L"\n");
    }

    return line.value_or(std::wstring{});
}

} // namespace wsl::windows::wslc
