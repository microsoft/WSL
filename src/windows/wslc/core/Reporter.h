/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.h

Abstract:

    Level-filtered user-facing output for the WSLC CLI.

    Each WriteLine/Write call is a single underlying WriteConsoleW or
    fwprintf+fflush, so concurrent calls cannot interleave mid-message.

    Reporter exposes a std::format-style API. Sequence / ConstructedSequence
    arguments are inspected per call: when the destination has VT disabled,
    every Sequence argument is replaced with an empty Sequence; when --no-color
    is set, only color-bearing Sequences are stripped. Cursor moves and other
    structural VT sequences continue to pass when VT is on.

--*/
#pragma once

#include "OutputChannel.h"
#include "VTSupport.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <Windows.h>

namespace wsl::windows::wslc {

namespace reporter_detail {

    // Pass-through for any non-Sequence argument: preserves value category so
    // the caller's lvalues stay as references inside the materialized tuple and
    // rvalues are moved in.
    template <typename T>
    constexpr T&& StripIfDisabled(T&& value, bool /*vtEnabled*/, bool /*colorEnabled*/) noexcept
    {
        return std::forward<T>(value);
    }

    // Sequence and ConstructedSequence both expose Get() and IsColor() through
    // the Sequence base. We return a Sequence by value that either borrows the
    // caller's bytes (when VT/color permits) or is empty (when stripped).
    // The borrow is safe because the caller's argument outlives the WriteLine
    // call (full-expression lifetime extension applies to temporaries).
    inline wsl::windows::common::vt::Sequence StripIfDisabled(const wsl::windows::common::vt::Sequence& sequence, bool vtEnabled, bool colorEnabled)
    {
        if (!vtEnabled)
        {
            return {};
        }
        if (!colorEnabled && sequence.IsColor())
        {
            return {};
        }
        return wsl::windows::common::vt::Sequence{sequence.Get()};
    }

    inline wsl::windows::common::vt::Sequence StripIfDisabled(const wsl::windows::common::vt::ConstructedSequence& sequence, bool vtEnabled, bool colorEnabled)
    {
        if (!vtEnabled)
        {
            return {};
        }
        if (!colorEnabled && sequence.IsColor())
        {
            return {};
        }
        return wsl::windows::common::vt::Sequence{sequence.Get()};
    }

} // namespace reporter_detail

struct Reporter
{
    enum class Level : uint32_t
    {
        None = 0x0,
        Debug = 0x1,   // verbose diagnostics; routes to stderr
        Output = 0x2,  // primary data output; routes to stdout
        Info = 0x4,    // diagnostic info; routes to stderr
        Warning = 0x8, // warnings; routes to stderr
        Error = 0x10,  // errors; routes to stderr
        Standard = Output | Info | Warning | Error,
        All = Debug | Output | Info | Warning | Error,
    };

    // Default: stdout/stderr, per-handle VT.
    Reporter();

    // Single FILE* for all output (no VT).
    explicit Reporter(FILE* outFile);

    // Output to outFile, diagnostics to errFile.
    Reporter(FILE* outFile, FILE* errFile);

    // Single FILE* with explicit VT.
    Reporter(FILE* outFile, bool vtEnabled);

    // Per-channel FILE* with explicit per-channel VT.
    Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled);

    NON_COPYABLE(Reporter);
    NON_MOVABLE(Reporter);

    ~Reporter() = default;

    // std::format-style write APIs. Each call emits exactly one underlying
    // WriteConsoleW or fwprintf+fflush. WriteLine appends a trailing newline;
    // Write does not. Sequence / ConstructedSequence arguments are stripped
    // when VT is disabled (or --no-color is set, for color-bearing sequences).
    template <typename... Args>
    void WriteLine(Level level, std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(level, /*appendNewline*/ true, std::move(fmt), std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Write(Level level, std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(level, /*appendNewline*/ false, std::move(fmt), std::forward<Args>(args)...);
    }

    // Per-level convenience wrappers; each appends a trailing newline.
    template <typename... Args>
    void Output(std::wformat_string<Args...> fmt, Args&&... args)
    {
        WriteLine(Level::Output, std::move(fmt), std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Info(std::wformat_string<Args...> fmt, Args&&... args)
    {
        WriteLine(Level::Info, std::move(fmt), std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Warn(std::wformat_string<Args...> fmt, Args&&... args)
    {
        WriteLine(Level::Warning, std::move(fmt), std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Error(std::wformat_string<Args...> fmt, Args&&... args)
    {
        WriteLine(Level::Error, std::move(fmt), std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Debug(std::wformat_string<Args...> fmt, Args&&... args)
    {
        WriteLine(Level::Debug, std::move(fmt), std::forward<Args>(args)...);
    }

    // True when the destination for this level honors VT escape sequences
    // (e.g. SGR color, cursor moves). Callers such as progress bars use this
    // to choose between animated and plain output.
    bool IsVTEnabled(Level level) const noexcept;

    // True when SGR color sequences will be emitted for this level
    // (= IsVTEnabled(level) && !no-color override).
    bool IsColorEnabled(Level level) const noexcept;

    // True when the user has not opted out of color via SetNoColor (does not
    // consider whether the destination supports VT).
    bool IsColorEnabled() const noexcept
    {
        return !m_noColor;
    }

    void SetNoColor(bool noColor) noexcept
    {
        m_noColor = noColor;
    }

    // Safe write width of the underlying console for this level (one column
    // reserved as an autowrap guard), or nullopt when the destination is
    // redirected or the screen-buffer query fails.
    std::optional<int> GetConsoleWidth(Level level) const;

    bool IsLevelEnabled(Level level) const noexcept;
    void SetLevelMask(Level level, bool enabled) noexcept;

    // Silences all further writes. Idempotent; safe to call from any thread.
    void Disable() noexcept;

private:
    const OutputChannel& ChannelFor(Level level) const noexcept
    {
        return (level == Level::Output) ? m_out : m_err;
    }

    // Sandwiches body with the level's SGR prefix + reset (when VT permits),
    // optionally appends '\n', and dispatches a single WriteString call.
    void Emit(const OutputChannel& channel, Level level, std::wstring_view body, bool appendNewline) const;

    template <typename... Args>
    void EmitFormatted(Level level, bool appendNewline, std::wformat_string<Args...> fmt, Args&&... args)
    {
        if (!m_enabled.load(std::memory_order_relaxed) || !IsLevelEnabled(level))
        {
            return;
        }

        const OutputChannel& channel = ChannelFor(level);
        const bool vtEnabled = channel.IsVTEnabled();
        const bool colorEnabled = vtEnabled && !m_noColor;

        // Materialize stripped args in a tuple so std::make_wformat_args can take
        // lvalue references to stable storage during vformat.
        auto stripped = std::tuple{reporter_detail::StripIfDisabled(std::forward<Args>(args), vtEnabled, colorEnabled)...};

        std::wstring body = std::apply(
            [&fmt](auto&... values) { return std::vformat(std::wstring_view{fmt.get()}, std::make_wformat_args(values...)); }, stripped);

        Emit(channel, level, body, appendNewline);
    }

    OutputChannel m_out;
    OutputChannel m_err;
    std::atomic_bool m_enabled{true};
    bool m_noColor = false;
    Level m_enabledLevels = Level::Standard;
};

DEFINE_ENUM_FLAG_OPERATORS(Reporter::Level);

} // namespace wsl::windows::wslc
