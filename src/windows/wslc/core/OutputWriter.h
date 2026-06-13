/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OutputWriter.h

Abstract:

    OutputChannel owns a write destination (console or FILE*).
    OutputWriter is a per-call, level-filtered, VT-formatted view over an OutputChannel.
    Each operator<< chain accumulates into a wstring buffer and flushes atomically
    on std::endl or std::flush, producing at most one WriteConsoleW() or fwprintf() call.

--*/
#pragma once

#include "VTSupport.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>

namespace wsl::windows::wslc {

using namespace wsl::windows::common::vt;

// Long-lived write destination used exclusively by Reporter.
// Console handle: WriteString() calls WriteConsoleW() directly.
// Redirected/FILE*: WriteString() calls fwprintf() without appending a newline.
struct OutputChannel
{
    NON_COPYABLE(OutputChannel);
    NON_MOVABLE(OutputChannel);

    // Used by test paths and redirected output.
    OutputChannel(FILE* file, bool vtEnabled);

    // Uses WriteConsoleW() on a real console; falls back to fallbackFile when redirected.
    OutputChannel(HANDLE handle, FILE* fallbackFile, bool vtEnabled);

    // Writes text in a single call. No-op when disabled or empty.
    void WriteString(std::wstring_view text);

    void SetVTEnabled(bool enabled);
    bool IsVTEnabled() const;

    // Silences all further writes. Called by Reporter::CloseOutputWriter(true).
    void Disable();

private:
    HANDLE m_consoleHandle = INVALID_HANDLE_VALUE; // valid on console path only
    FILE* m_file = nullptr;                        // non-null on FILE* path only
    std::atomic_bool m_enabled = true;
    bool m_VTEnabled;
};

// Per-call writer returned by Reporter::Info(), Warn(), Error(), Debug().
// Accumulates operator<< output into a wstring; flushes atomically on std::endl or std::flush.
// Destructor flushes as a safety net if no explicit flush manipulator was used.
struct OutputWriter
{
    // enabled controls whether any output is produced. false = suppress all writes and flush.
    OutputWriter(OutputChannel& out, bool enabled = true, bool vtEnabled = true, bool colorEnabled = true);

    NON_COPYABLE(OutputWriter);

    // Move assignment is intentionally deleted.
    OutputWriter(OutputWriter&& other) noexcept;
    OutputWriter& operator=(OutputWriter&&) = delete;

    ~OutputWriter();

    void AddFormat(const Sequence& sequence);
    void ClearFormat();

    template <typename T>
    OutputWriter& operator<<(const T& t)
    {
        if (!m_enabled)
        {
            return *this;
        }
        if (m_VTEnabled)
        {
            ApplyFormat();
        }
        AppendToBuffer(t);
        MarkWritten();
        return *this;
    }

    OutputWriter& operator<<(std::wostream&(__cdecl* f)(std::wostream&));
    OutputWriter& operator<<(const Sequence& sequence);
    OutputWriter& operator<<(const ConstructedSequence& sequence);
    OutputWriter& operator<<(const std::filesystem::path& path);

private:
    void ApplyFormat();
    void Flush();

    // Records that new content has been appended to m_buffer.
    // Resets m_flushed so a subsequent Flush() (including the destructor's
    // safety-net flush) will emit the post-flush content instead of skipping
    // it as already-flushed.
    void MarkWritten()
    {
        m_written = true;
        m_flushed = false;
    }

    // AppendToBuffer accepts only:
    //   - types convertible to std::wstring_view (std::wstring, const wchar_t*, etc.)
    //   - wchar_t
    //   - bool (rendered as L"true"/L"false")
    //   - integral and floating-point arithmetic types
    //
    // Narrow types (std::string, const char*, char) and arbitrary streamable types
    // are rejected at compile time to avoid locale-dependent narrow-to-wide
    // conversions. Callers must convert narrow text explicitly via
    // wsl::shared::string::MultiByteToWide().
    //
    // To support a new type, extend the branches below.
    template <typename T>
    void AppendToBuffer(const T& t)
    {
        if constexpr (std::is_convertible_v<const T&, std::wstring_view>)
        {
            m_buffer.append(std::wstring_view(t));
        }
        else if constexpr (std::is_same_v<T, wchar_t>)
        {
            m_buffer += t;
        }
        else if constexpr (std::is_same_v<T, char> || std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char>)
        {
            // Narrow character types satisfy std::is_integral_v and would otherwise
            // render as their numeric code-point ("a" -> "97").
            static_assert(!sizeof(T*), "OutputWriter does not accept narrow character types. Convert to wchar_t or wide text.");
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            m_buffer.append(t ? L"true" : L"false");
        }
        else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>)
        {
            m_buffer.append(std::to_wstring(t));
        }
        else
        {
            // sizeof-dependent expression defers the assertion to instantiation.
            static_assert(!sizeof(T*), "OutputWriter has no overload for this type. Convert narrow strings to wide text.");
        }
    }

    OutputChannel& m_out;
    bool m_enabled;
    bool m_VTEnabled;
    bool m_colorEnabled;
    bool m_written = false;
    bool m_flushed = false;
    bool m_colorWritten = false;

    // m_formatDelay starts at 1 so the level format fires on the first text write.
    // Set to 2 after a caller Sequence so the level format follows one write later.
    size_t m_formatDelay = 1;

    ConstructedSequence m_format;
    std::wstring m_buffer;
};

} // namespace wsl::windows::wslc
