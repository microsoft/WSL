/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Terminal.h

Abstract:

    Level-filtered, std::format-style user-facing output for the WSLC CLI, plus
    line-oriented user input (prompts). Sequence arguments are stripped when VT is
    off; color Sequences are also stripped when color is disabled, while cursor-move
    Sequences still pass through.

--*/
#pragma once

#include "InputChannel.h"
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

// Fallback width for progress displays when the console width can't be queried. This
// value already includes the autowrap guard (visible width minus one) so a wrapped line
// can't corrupt cursor-based rendering.
inline constexpr int c_fallbackConsoleWidth = 79;

namespace terminal_detail {

    // SFINAE: excludes Sequence-derived types so the overload below wins for them.
    template <typename T, typename = std::enable_if_t<!std::is_base_of_v<wsl::windows::common::vt::Sequence, std::remove_cvref_t<T>>>>
    constexpr T&& StripIfDisabled(T&& value, bool /*vtEnabled*/, bool /*colorEnabled*/) noexcept
    {
        return std::forward<T>(value);
    }

    // Returns VT bytes when permitted, empty when stripped. The returned view borrows
    // from the caller's argument, which outlives the Write call.
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

} // namespace terminal_detail

struct Terminal
{
    enum class Level
    {
        Output,
        Info,
        Warning,
        Error,
    };

    Terminal();
    Terminal(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled, FILE* inFile = nullptr, bool inInteractive = false);

    NON_COPYABLE(Terminal);
    NON_MOVABLE(Terminal);

    ~Terminal() = default;

    // std::format-style write API.
    template <typename... Args>
    void Write(Level level, std::wformat_string<Args...> fmt, Args&&... args)
    {
        EmitFormatted(level, std::move(fmt), std::forward<Args>(args)...);
    }

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

    // True when user input is attached to an interactive console (a prompt can be
    // shown and echo can be masked); false when input is redirected from a file or pipe.
    bool IsInputInteractive() const noexcept
    {
        return m_in.IsInteractive();
    }

    // Reads a single line of user input, stripping the trailing CR and/or LF. Returns
    // nullopt at end of input with nothing read. When mask is true and input is an
    // interactive console, echo is disabled for the duration of the read.
    std::optional<std::wstring> ReadLine(bool mask = false)
    {
        return m_in.ReadLine(mask);
    }

    // Writes label (no trailing newline) at the given level, then reads a line of input.
    // When mask is true and input is interactive, echo is disabled during the read and a
    // trailing newline is emitted afterward (the un-echoed Enter). Returns the line, or an
    // empty string at end of input.
    std::wstring PromptForLine(Level level, std::wstring_view label, bool mask);

    // Convenience overload that defaults prompts to stdout (Level::Output) to align with the
    // container CLI ecosystem: Docker (cli.Out()), containerd/nerdctl (cmd.OutOrStdout()), and
    // Apple container (Swift print) all prompt on stdout. This diverges from general Unix tools
    // (sudo, ssh, git, gh) that prompt on stderr/tty to keep stdout pipeable, but WSLC follows
    // Docker's CLI semantics.
    std::wstring PromptForLine(std::wstring_view label, bool mask = false)
    {
        return PromptForLine(Level::Output, label, mask);
    }

    bool IsVTEnabled(Level level) const noexcept;

    bool IsColorEnabled(Level level) const noexcept;

    bool IsNoColor() const noexcept
    {
        return m_noColor;
    }

    void SetNoColor(bool noColor) noexcept
    {
        m_noColor = noColor;
    }

    // Console write width minus one (autowrap guard), or nullopt when redirected.
    std::optional<int> GetConsoleWidth(Level level) const;

private:
    const OutputChannel& ChannelFor(Level level) const noexcept
    {
        return (level == Level::Output) ? m_out : m_err;
    }

    // Per-level SGR prefix (empty when color is off).
    std::wstring_view LevelPrefix(Level level) const noexcept;

    template <typename... Args>
    void EmitFormatted(Level level, std::wformat_string<Args...> fmt, Args&&... args)
    {
        const OutputChannel& channel = ChannelFor(level);
        const bool vtEnabled = channel.IsVTEnabled();
        const bool colorEnabled = vtEnabled && !m_noColor;

        // Materialize stripped args into stable storage for vformat.
        auto stripped = std::tuple{terminal_detail::StripIfDisabled(std::forward<Args>(args), vtEnabled, colorEnabled)...};

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
    InputChannel m_in;
    bool m_noColor = false;
};

} // namespace wsl::windows::wslc
