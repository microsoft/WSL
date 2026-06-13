/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.h

Abstract:

    Level-filtered user-facing output for the WSLC CLI.
    Info goes to stdout; Debug/Warning/Error go to stderr.

--*/
#pragma once
#include "OutputWriter.h"

#include <memory>

namespace wsl::windows::wslc {

struct Reporter
{
    enum class Level : uint32_t
    {
        None = 0x0,
        Debug = 0x1,
        Info = 0x2,
        Warning = 0x4,
        Error = 0x8,
        Standard = Info | Warning | Error,
        All = Debug | Info | Warning | Error,
    };

    // Default: stdout/stderr, VT per handle.
    Reporter();

    // Single FILE* for all output.
    Reporter(FILE* outFile);

    // Info to outFile, diagnostics to errFile.
    Reporter(FILE* outFile, FILE* errFile);

    // Single FILE* with explicit VT.
    Reporter(FILE* outFile, bool vtEnabled);

    // Per-channel FILE* and VT.
    Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled);

    NON_COPYABLE(Reporter);

    Reporter(Reporter&&) = default;
    Reporter& operator=(Reporter&&) = default;

    ~Reporter();

    bool IsColorEnabled() const;
    void SetNoColor(bool noColor);

    OutputWriter Debug();
    OutputWriter Info();
    OutputWriter Warn();
    OutputWriter Error();

    OutputWriter GetOutputWriter(Level level);

    void CloseOutputWriter(bool forceDisable = false);

    bool IsLevelEnabled(Level level) const;
    void SetLevelMask(Level level, bool setEnabled);

private:
    Reporter(std::shared_ptr<OutputChannel> outChannel, std::shared_ptr<OutputChannel> errChannel);

    std::shared_ptr<OutputChannel> m_out;
    std::shared_ptr<OutputChannel> m_err;
    bool m_noColor = false;
    Level m_enabledLevels = Level::Standard;
};

DEFINE_ENUM_FLAG_OPERATORS(Reporter::Level);

} // namespace wsl::windows::wslc
