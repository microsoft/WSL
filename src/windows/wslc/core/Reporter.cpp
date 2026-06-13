/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.cpp

Abstract:

    Implementation of Reporter.

--*/
#include "precomp.h"
#include "Reporter.h"
#include <iostream>

namespace wsl::windows::wslc {

using namespace wsl::windows::common::vt;

namespace {
    // Caller must enable VT on the matching handle before constructing a Reporter.
    bool QueryVTEnabled(HANDLE handle)
    {
        DWORD mode = 0;
        return handle != INVALID_HANDLE_VALUE && handle != nullptr && GetConsoleMode(handle, &mode) &&
               WI_IsFlagSet(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
} // namespace

Reporter::Reporter() :
    Reporter(
        std::make_shared<OutputChannel>(GetStdHandle(STD_OUTPUT_HANDLE), stdout, QueryVTEnabled(GetStdHandle(STD_OUTPUT_HANDLE))),
        std::make_shared<OutputChannel>(GetStdHandle(STD_ERROR_HANDLE), stderr, QueryVTEnabled(GetStdHandle(STD_ERROR_HANDLE))))
{
}

Reporter::Reporter(FILE* outFile) :
    Reporter(std::make_shared<OutputChannel>(outFile, false), std::make_shared<OutputChannel>(outFile, false))
{
}

Reporter::Reporter(FILE* outFile, FILE* errFile) :
    Reporter(std::make_shared<OutputChannel>(outFile, false), std::make_shared<OutputChannel>(errFile, false))
{
}

Reporter::Reporter(FILE* outFile, bool vtEnabled) :
    Reporter(std::make_shared<OutputChannel>(outFile, vtEnabled), std::make_shared<OutputChannel>(outFile, vtEnabled))
{
}

Reporter::Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled) :
    Reporter(std::make_shared<OutputChannel>(outFile, outVtEnabled), std::make_shared<OutputChannel>(errFile, errVtEnabled))
{
}

Reporter::Reporter(std::shared_ptr<OutputChannel> outChannel, std::shared_ptr<OutputChannel> errChannel) :
    m_out(std::move(outChannel)), m_err(std::move(errChannel))
{
}

Reporter::~Reporter()
{
    if (m_out)
    {
        CloseOutputWriter();
    }
}

bool Reporter::IsColorEnabled() const
{
    return !m_noColor;
}

void Reporter::SetNoColor(bool noColor)
{
    m_noColor = noColor;
}

OutputWriter Reporter::Debug()
{
    return GetOutputWriter(Level::Debug);
}

OutputWriter Reporter::Info()
{
    return GetOutputWriter(Level::Info);
}

OutputWriter Reporter::Warn()
{
    return GetOutputWriter(Level::Warning);
}

OutputWriter Reporter::Error()
{
    return GetOutputWriter(Level::Error);
}

OutputWriter Reporter::GetOutputWriter(Level level)
{
    if (WI_AreAllFlagsClear(m_enabledLevels, level))
    {
        return OutputWriter(*m_out, false); // enabled=false suppresses all output
    }

    // Only Info — the program's actual output — goes to stdout. Everything
    // else (Debug, Warning, Error) is diagnostic chatter about the program
    // running and goes to stderr, matching gcc/clang/git/cargo/kubectl/etc.
    // This keeps `wslc cmd | jq` and `wslc cmd > out.txt` clean regardless of
    // the active level mask. The target channel's VT capability still drives
    // whether SGR sequences are emitted, so a redirected stderr stays plain
    // even when stdout is a TTY.
    OutputChannel& target = (level == Level::Info) ? *m_out : *m_err;
    const bool vtEnabled = target.IsVTEnabled();
    const bool colorEnabled = vtEnabled && !m_noColor;
    OutputWriter result{target, true, vtEnabled, colorEnabled};

    switch (level)
    {
    case Level::Debug:
        result.AddFormat(Format::Dim);
        break;
    case Level::Info:
        result.AddFormat(Format::Default);
        break;
    case Level::Warning:
        result.AddFormat(Format::Fg::BrightYellow);
        break;
    case Level::Error:
        result.AddFormat(Format::Fg::BrightRed);
        break;
    default:
        THROW_HR(E_UNEXPECTED);
    }

    return result;
}

void Reporter::CloseOutputWriter(bool forceDisable)
{
    if (!m_out)
    {
        return;
    }

    if (forceDisable)
    {
        m_out->Disable();
        m_err->Disable();
    }
}

bool Reporter::IsLevelEnabled(Level level) const
{
    return WI_AreAllFlagsSet(m_enabledLevels, level);
}

void Reporter::SetLevelMask(Level level, bool setEnabled)
{
    if (setEnabled)
    {
        WI_SetAllFlags(m_enabledLevels, level);
    }
    else
    {
        WI_ClearAllFlags(m_enabledLevels, level);
    }
}

} // namespace wsl::windows::wslc
