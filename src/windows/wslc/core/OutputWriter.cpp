/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OutputWriter.cpp

Abstract:

    Implementation of OutputChannel and OutputWriter.

--*/
#include "precomp.h"
#include "OutputWriter.h"

namespace wsl::windows::wslc {

using namespace wsl::windows::common::vt;

OutputChannel::OutputChannel(FILE* file, bool vtEnabled) : m_file(file), m_VTEnabled(vtEnabled)
{
    WI_ASSERT(m_file);
}

OutputChannel::OutputChannel(HANDLE handle, FILE* fallbackFile, bool vtEnabled) : m_file(fallbackFile), m_VTEnabled(vtEnabled)
{
    // Switch to WriteConsoleW path only when the handle is a real console.
    // Redirected handles (pipe, file) use fwprintf on m_file instead.
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr && GetConsoleMode(handle, &mode))
    {
        m_consoleHandle = handle;
        m_file = nullptr;
    }
    else
    {
        // Non-console handle: we'll fall through to fwprintf on m_file.
        WI_ASSERT(m_file);
    }
}

void OutputChannel::WriteString(std::wstring_view text)
{
    if (!m_enabled || text.empty())
    {
        return;
    }

    if (m_consoleHandle != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        LOG_IF_WIN32_BOOL_FALSE(WriteConsoleW(m_consoleHandle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
    }
    else
    {
        // No newline appended; the buffer already contains all desired formatting.
        if (fwprintf(m_file, L"%.*ls", static_cast<int>(text.size()), text.data()) < 0)
        {
            const int err = errno;
            LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), "fwprintf to redirected output failed (errno=%d)", err);
        }

        // Redirected stdout/stderr are fully buffered by the CRT; flush so
        // std::endl/std::flush reach the OS as the writer's contract promises.
        if (fflush(m_file) != 0)
        {
            const int err = errno;
            LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), "fflush of redirected output failed (errno=%d)", err);
        }
    }
}

void OutputChannel::SetVTEnabled(bool enabled)
{
    m_VTEnabled = enabled;
}

bool OutputChannel::IsVTEnabled() const
{
    return m_VTEnabled;
}

void OutputChannel::Disable()
{
    m_enabled = false;
}

OutputWriter::OutputWriter(OutputChannel& out, bool enabled, bool vtEnabled, bool colorEnabled) :
    m_out(out), m_enabled(enabled), m_VTEnabled(vtEnabled), m_colorEnabled(colorEnabled)
{
}

OutputWriter::OutputWriter(OutputWriter&& other) noexcept :
    m_out(other.m_out),
    m_enabled(other.m_enabled),
    m_VTEnabled(other.m_VTEnabled),
    m_colorEnabled(other.m_colorEnabled),
    m_written(other.m_written),
    m_flushed(other.m_flushed),
    m_colorWritten(other.m_colorWritten),
    m_formatDelay(other.m_formatDelay),
    m_format(std::move(other.m_format)),
    m_buffer(std::move(other.m_buffer))
{
    other.m_enabled = false;
    other.m_written = false;
    other.m_flushed = true;
    other.m_colorWritten = false;
}

OutputWriter::~OutputWriter()
{
    Flush();
}

void OutputWriter::Flush()
{
    if (!m_written || m_flushed)
    {
        return;
    }

    if (m_colorWritten)
    {
        // Insert reset before any trailing newline so SGR state is cleared
        // before the cursor advances, matching normal terminal conventions.
        if (!m_buffer.empty() && m_buffer.back() == L'\n')
        {
            m_buffer.pop_back();
            m_buffer.append(Format::Default.Get());
            m_buffer += L'\n';
        }
        else
        {
            m_buffer.append(Format::Default.Get());
        }
    }

    m_out.WriteString(m_buffer);
    m_buffer.clear();
    m_flushed = true;
    // m_written is intentionally left set so MarkWritten() can distinguish a
    // post-flush re-append (which must clear m_flushed) from a no-op flush.
    m_colorWritten = false;
    m_formatDelay = 1;
}

void OutputWriter::AddFormat(const Sequence& sequence)
{
    m_format.Append(sequence);
}

void OutputWriter::ClearFormat()
{
    m_format.Clear();
}

void OutputWriter::ApplyFormat()
{
    if (!m_colorEnabled)
    {
        m_formatDelay = 0;
        return;
    }

    if (m_formatDelay && !--m_formatDelay)
    {
        const auto sv = m_format.Get();
        if (!sv.empty())
        {
            m_buffer.append(sv);
            m_colorWritten = true;
        }
    }
}

OutputWriter& OutputWriter::operator<<(std::wostream&(__cdecl* f)(std::wostream&))
{
    if (!m_enabled)
    {
        return *this;
    }
    if (f == static_cast<std::wostream&(__cdecl*)(std::wostream&)>(std::endl))
    {
        m_buffer += L'\n';
        MarkWritten();
        Flush();
    }
    else if (f == static_cast<std::wostream&(__cdecl*)(std::wostream&)>(std::flush))
    {
        Flush();
    }

    return *this;
}

OutputWriter& OutputWriter::operator<<(const Sequence& sequence)
{
    if (!m_enabled || !m_VTEnabled)
    {
        return *this;
    }

    if (!m_colorEnabled && sequence.IsColor())
    {
        return *this;
    }

    ApplyFormat();
    m_buffer.append(sequence.Get());
    if (sequence.IsColor())
    {
        m_colorWritten = true;
    }
    m_formatDelay = 2;
    MarkWritten();

    return *this;
}

OutputWriter& OutputWriter::operator<<(const ConstructedSequence& sequence)
{
    if (!m_enabled || !m_VTEnabled)
    {
        return *this;
    }

    if (!m_colorEnabled && sequence.IsColor())
    {
        return *this;
    }

    ApplyFormat();
    m_buffer.append(sequence.Get());
    if (sequence.IsColor())
    {
        m_colorWritten = true;
    }
    m_formatDelay = 2;
    MarkWritten();

    return *this;
}

OutputWriter& OutputWriter::operator<<(const std::filesystem::path& path)
{
    if (!m_enabled)
    {
        return *this;
    }
    if (m_VTEnabled)
    {
        ApplyFormat();
    }
    m_buffer.append(path.native());
    MarkWritten();
    return *this;
}

} // namespace wsl::windows::wslc
