/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.h

Abstract:

    Central user-facing output facility for the WSLC CLI.
    Provides level-filtered writers with optional VT color formatting.

    Ownership:
      Reporter owns two OutputChannel instances (stdout and stderr).
      OutputChannel owns the write destination: WriteConsoleW on a real
      console, or fwprintf on a redirected FILE*.

    Per-call flow:
      Info(), Warn(), Error(), Debug() each return a short-lived OutputWriter
      that borrows a reference to the appropriate OutputChannel. The OutputWriter
      accumulates the entire operator<< chain into a wstring buffer and flushes
      it atomically on std::endl or std::flush, producing at most one
      WriteConsoleW() or fwprintf() call per chain.

    Routing:
      Info goes to the out channel. Debug, Warning, and Error go to the err
      channel. VT/color is decided per channel based on its destination, so
      redirecting one stream does not affect formatting on the other.

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

    // Default console reporter: Info → stdout, Debug/Warning/Error → stderr.
    // VT/color is enabled per-handle based on the current console mode.
    Reporter();

    // Test constructor: both Info and diagnostics go to outFile (single-pipe capture).
    // VT/color disabled.
    Reporter(FILE* outFile);

    // Info writes to outFile; Debug/Warning/Error write to errFile.
    // Each channel's VT/color state is derived independently from its FILE*,
    // so a redirected errFile stays plain even when outFile is a TTY.
    Reporter(FILE* outFile, FILE* errFile);

    // Test constructor: outFile with explicit VT control.
    Reporter(FILE* outFile, bool vtEnabled);

    // Test constructor: independent FILE* and VT state per channel.
    Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled);

    NON_COPYABLE(Reporter);

    Reporter(Reporter&&) = default;
    Reporter& operator=(Reporter&&) = default;

    ~Reporter();

    bool IsColorEnabled() const;

    // Suppresses SGR color and hyperlink sequences. Does not affect IsVTEnabled().
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
