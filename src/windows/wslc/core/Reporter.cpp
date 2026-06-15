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
    return GetWriter(Level::Debug);
}

OutputWriter Reporter::Output()
{
    return GetWriter(Level::Output);
}

OutputWriter Reporter::Info()
{
    return GetWriter(Level::Info);
}

OutputWriter Reporter::Warn()
{
    return GetWriter(Level::Warning);
}

OutputWriter Reporter::Error()
{
    return GetWriter(Level::Error);
}

OutputWriter Reporter::GetWriter(Level level)
{
    if (WI_AreAllFlagsClear(m_enabledLevels, level))
    {
        return OutputWriter(*m_out, false);
    }

    // Output to stdout; diagnostics to stderr. Per-channel VT decides SGR emission.
    OutputChannel& target = (level == Level::Output) ? *m_out : *m_err;
    const bool vtEnabled = target.IsVTEnabled();
    const bool colorEnabled = vtEnabled && !m_noColor;
    OutputWriter result{target, true, vtEnabled, colorEnabled};

    switch (level)
    {
    case Level::Debug:
        result.AddFormat(Format::Dim);
        break;
    case Level::Output:
        result.AddFormat(Format::Default);
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

bool Reporter::IsVTEnabled(Level level) const
{
    return (level == Level::Output ? m_out : m_err)->IsVTEnabled();
}

std::optional<int> Reporter::GetConsoleWidth(Level level) const
{
    return (level == Level::Output ? m_out : m_err)->GetConsoleWidth();
}

} // namespace wsl::windows::wslc
