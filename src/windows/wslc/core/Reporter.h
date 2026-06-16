/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.h

Abstract:

    Level-filtered user-facing output for the WSLC CLI.

    Each Write call is a single underlying WriteConsoleW or fwprintf,
    so concurrent calls cannot interleave mid-message.

    Reporter exposes a std::format-style API. Sequence / ConstructedSequence
    arguments are inspected per call: when the destination has VT disabled,
    every Sequence argument is replaced with an empty Sequence; when --no-color
    is set, only color-bearing Sequences are stripped. Cursor moves and other
    structural VT sequences continue to pass when VT is on.

--*/
#pragma once

#include "OutputChannel.h"
#include "VTSupport.h"

#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace wsl::windows::wslc {

namespace reporter_detail {

    // Pass-through for any non-Sequence argument: preserves value category so
    // the caller's lvalues stay as references inside the materialized tuple and
    // rvalues are moved in. SFINAE excludes Sequence-derived types so the
    // Sequence overload below is selected for them.
    template <typename T, typename = std::enable_if_t<!std::is_base_of_v<wsl::windows::common::vt::Sequence, std::remove_cvref_t<T>>>>
    constexpr T&& StripIfDisabled(T&& value, bool /*vtEnabled*/, bool /*colorEnabled*/) noexcept
    {
        return std::forward<T>(value);
    }

    // Handles both Sequence and ConstructedSequence (which derives from
    // Sequence). Returns the VT bytes as a string_view when permitted, or
    // empty when stripped. The borrow is safe because the caller's argument
    // outlives the Write call (full-expression lifetime extension applies
    // to temporaries).
    inline std::wstring_view StripIfDisabled(const wsl::windows::common::vt::Sequence& sequence, bool vtEnabled, bool colorEnabled)
    {
        if (!vtEnabled)
        {
            return {};
        }
        if (!colorEnabled && sequence.IsColor())
        {
            return {};
        }
        return sequence.Get();
    }

} // namespace reporter_detail

struct Reporter
{
    enum class Level
    {
        Output,  // primary data output; routes to stdout
        Info,    // diagnostic info; routes to stderr
        Warning, // warnings; routes to stderr
        Error,   // errors; routes to stderr
    };

    // Default: stdout/stderr, per-handle VT.
    Reporter();

    // Per-channel FILE* with explicit per-channel VT (for tests).
    Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled);

    NON_COPYABLE(Reporter);
    NON_MOVABLE(Reporter);

    ~Reporter() = default;

    // std::format-style write API. Each call emits exactly one underlying
    // WriteConsoleW or fwprintf+fflush. Sequence / ConstructedSequence
    // arguments are stripped when VT is disabled (or no-color is set, for
    // color-bearing sequences).
    template <typename... Args>
    void Write(Level level, std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(level, std::move(fmt), std::forward<Args>(args)...);
    }

    // Per-level convenience wrappers.
    template <typename... Args>
    void Output(std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(Level::Output, std::move(fmt), std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Info(std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(Level::Info, std::move(fmt), std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Warn(std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(Level::Warning, std::move(fmt), std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Error(std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(Level::Error, std::move(fmt), std::forward<Args>(args)...);
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

private:
    const OutputChannel& ChannelFor(Level level) const noexcept
    {
        return (level == Level::Output) ? m_out : m_err;
    }

    // Per-level SGR colorant (empty when color is off).
    std::wstring_view LevelPrefix(Level level) const noexcept;

    template <typename... Args>
    void EmitFormatted(Level level, std::wformat_string<Args...> fmt, Args&&... args)
    {
        const OutputChannel& channel = ChannelFor(level);
        const bool vtEnabled = channel.IsVTEnabled();
        const bool colorEnabled = vtEnabled && !m_noColor;

        // Materialize stripped args in a tuple so std::make_wformat_args can take
        // lvalue references to stable storage during vformat.
        auto stripped = std::tuple{reporter_detail::StripIfDisabled(std::forward<Args>(args), vtEnabled, colorEnabled)...};

        std::wstring body = std::apply(
            [&fmt](auto&... values) { return std::vformat(std::wstring_view{fmt.get()}, std::make_wformat_args(values...)); }, stripped);

        const auto prefix = LevelPrefix(level);
        if (prefix.empty())
        {
            channel.WriteString(body);
            return;
        }

        const auto reset = wsl::windows::common::vt::Format::Default.Get();
        std::wstring out;
        out.reserve(prefix.size() + body.size() + reset.size());
        out.append(prefix);
        out.append(body);
        out.append(reset);
        channel.WriteString(out);
    }

    OutputChannel m_out;
    OutputChannel m_err;
    bool m_noColor = false;
};

} // namespace wsl::windows::wslc
