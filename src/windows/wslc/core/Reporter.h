/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.h

Abstract:

    Level-filtered user-facing output for the WSLC CLI.
    Output goes to stdout; Debug/Info/Warning/Error go to stderr.

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
        Debug = 0x1,   // Verbose diagnostics, routes to stderr
        Output = 0x2,  // Primary data output, routes to stdout
        Info = 0x4,    // Diagnostic info, routes to stderr
        Warning = 0x8, // Warnings, routes to stderr
        Error = 0x10,  // Errors, routes to stderr
        Standard = Output | Info | Warning | Error,
        All = Debug | Output | Info | Warning | Error,
    };

    // Default: stdout/stderr, VT per handle.
    Reporter();

    // Single FILE* for all output.
    Reporter(FILE* outFile);

    // Output to outFile, diagnostics to errFile.
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
    OutputWriter Output();
    OutputWriter Info();
    OutputWriter Warn();
    OutputWriter Error();

    OutputWriter GetOutputWriter(Level level);

    void CloseOutputWriter(bool forceDisable = false);

    bool IsLevelEnabled(Level level) const;
    void SetLevelMask(Level level, bool setEnabled);

    // True when the destination channel for the given level supports VT (cursor moves, color).
    // Callers (e.g. progress callbacks) use this to choose between animated and plain output.
    bool IsVTEnabled(Level level) const;

    // Safe-write width in columns of the destination console for the given level, or
    // std::nullopt when the destination is redirected. Callers that perform width-based
    // truncation should skip it when this returns std::nullopt.
    std::optional<int> GetConsoleWidth(Level level) const;

private:
    Reporter(std::shared_ptr<OutputChannel> outChannel, std::shared_ptr<OutputChannel> errChannel);

    std::shared_ptr<OutputChannel> m_out;
    std::shared_ptr<OutputChannel> m_err;
    bool m_noColor = false;
    Level m_enabledLevels = Level::Standard;
};

DEFINE_ENUM_FLAG_OPERATORS(Reporter::Level);

} // namespace wsl::windows::wslc
