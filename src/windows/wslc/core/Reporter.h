/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Reporter.h

Abstract:

    Level-filtered, std::format-style user-facing output for the WSLC CLI.
    Sequence arguments are stripped when VT is off; color Sequences are also
    stripped when color is disabled, while cursor-move Sequences still pass
    through.

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

} // namespace reporter_detail

struct Reporter
{
    enum class Level
    {
        Output,
        Info,
        Warning,
        Error,
    };

    Reporter();
    Reporter(FILE* outFile, bool outVtEnabled, FILE* errFile, bool errVtEnabled);

    NON_COPYABLE(Reporter);
    NON_MOVABLE(Reporter);

    ~Reporter() = default;

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
