/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCTestHelpers.h

Abstract:

    Helper utilities for WSLC CLI unit tests.

--*/

#pragma once

#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <WexTestClass.h>
#include <wil/resource.h>
#include <wslutil.h>
#include "windows/Common.h"
#include "Invocation.h"
#include "OutputChannel.h"
#include "Reporter.h"
#include "TableOutput.h"

namespace WSLCTestHelpers {

inline wsl::windows::wslc::Invocation CreateInvocationFromCommandLine(const std::wstring& commandLine)
{
    // Simulate creation of Arvc/Argc from command line as Windows does.
    int argc = 0;
    wil::unique_hlocal_ptr<LPWSTR[]> argv;
    argv.reset(CommandLineToArgvW(commandLine.c_str(), &argc));
    VERIFY_IS_NOT_NULL(argv.get());
    VERIFY_IS_GREATER_THAN(argc, 0);

    // Convert to vector for Invocation, skipping argv[0] (executable path)
    // This is what we do in wmain() to populate Invocation input vector.
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) // Skip argv[0]
    {
        args.push_back(argv[i]);
    }

    return wsl::windows::wslc::Invocation(std::move(args));
}

// Helper function to convert wstring to UTF-8 string for TAEF logging
inline std::string WStringToUTF8(const std::wstring& wstr)
{
    if (wstr.empty())
    {
        return std::string();
    }

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &result[0], size_needed, nullptr, nullptr);
    return result;
}

// Convenience wrapper for Log::Comment with wstring
inline void LogComment(const std::wstring& message)
{
    WEX::Logging::Log::Comment(reinterpret_cast<const char8_t*>(WStringToUTF8(message).c_str()));
}

// RAII pipe pair for capturing FILE* output in tests.
// file() is passed to OutputChannel/Reporter; captured() drains the read end after flush.
struct CapturePipe
{
    CapturePipe()
    {
        // ReadPipeOverlapped=true so PartialHandleRead's InterruptableRead can be
        // interrupted by m_exitEvent during teardown if fclose hasn't run yet.
        auto [r, w] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
        wil::unique_handle writeHandle{w.release()};
        m_file = FileFromHandle(writeHandle, "w");

        const int fd = _fileno(m_file.get());
        WI_VERIFY(_setmode(fd, _O_U8TEXT) != -1);

        // Disable CRT buffering so each fwprintf is a single write. Prevents
        // _O_U8TEXT from splitting VT escape sequences across buffer flushes.
        setvbuf(m_file.get(), nullptr, _IONBF, 0);

        // CapturePipe owns the read pipe; PartialHandleRead borrows it via .get().
        // m_readPipe is declared before m_reader so destruction order tears the reader
        // down first (joining its thread) and only then closes the handle it was reading.
        m_readPipe = std::move(r);
        m_reader = std::make_unique<PartialHandleRead>(m_readPipe.get());
    }

    NON_COPYABLE(CapturePipe);
    NON_MOVABLE(CapturePipe);

    FILE* file() const
    {
        return m_file.get();
    }

    std::wstring captured()
    {
        m_file.reset();

        m_reader->ExpectClosed();
        std::wstring result = wsl::shared::string::MultiByteToWide(m_reader->GetData());

        // _O_U8TEXT prepends a UTF-8 BOM on some streams; strip it if present.
        if (!result.empty() && result[0] == L'\xFEFF')
        {
            result.erase(0, 1);
        }

        // _O_U8TEXT translates \n to \r\n; strip \r so tests compare plain newlines.
        result.erase(std::remove(result.begin(), result.end(), L'\r'), result.end());
        return result;
    }

private:
    wil::unique_file m_file;
    wil::unique_hfile m_readPipe;
    std::unique_ptr<PartialHandleRead> m_reader;
};

// Reporter wired to a single capture pipe for full output capture.
// VT is disabled (not a console handle), so error output stays in the same pipe.
struct CaptureReporter
{
    CapturePipe pipe;
    wsl::windows::wslc::Reporter reporter;

    explicit CaptureReporter(bool vtEnabled = false) : reporter(pipe.file(), vtEnabled, pipe.file(), vtEnabled)
    {
    }

    std::wstring captured()
    {
        return pipe.captured();
    }
};

// Helper: capture all lines emitted by a TableOutput into a vector<wstring>.
template <size_t N>
struct TableOutputCapture
{
    CaptureReporter capture;
    wsl::windows::wslc::TableOutput<N> table;

    // Header + optional config + optional VT flag.
    explicit TableOutputCapture(
        typename wsl::windows::wslc::TableOutput<N>::header_t&& header,
        size_t sizingBuffer = 50,
        size_t columnPadding = wsl::windows::wslc::TableOutput<N>::DefaultColumnPadding,
        bool vtEnabled = false) :
        capture(vtEnabled), table(capture.reporter, std::move(header), sizingBuffer, columnPadding)
    {
        table.SetConsoleWidthOverride(120);
    }

    // Header + column configs + optional VT flag.
    explicit TableOutputCapture(
        typename wsl::windows::wslc::TableOutput<N>::header_t&& header,
        typename wsl::windows::wslc::TableOutput<N>::column_config_t&& configs,
        bool vtEnabled = false) :
        capture(vtEnabled),
        table(capture.reporter, std::move(header), std::move(configs), 50, wsl::windows::wslc::TableOutput<N>::DefaultColumnPadding)
    {
        table.SetConsoleWidthOverride(120);
    }

    // Column definitions.
    explicit TableOutputCapture(typename wsl::windows::wslc::TableOutput<N>::column_def_t&& defs, bool vtEnabled = false) :
        capture(vtEnabled), table(capture.reporter, std::move(defs))
    {
        table.SetConsoleWidthOverride(120);
    }

    // Returns captured output split into lines.
    std::vector<std::wstring> lines()
    {
        auto raw = capture.captured();
        std::vector<std::wstring> result;
        size_t pos = 0;
        while (pos < raw.size())
        {
            auto nl = raw.find(L'\n', pos);
            if (nl == std::wstring::npos)
            {
                result.emplace_back(raw.substr(pos));
                break;
            }
            result.emplace_back(raw.substr(pos, nl - pos));
            pos = nl + 1;
        }
        // Remove trailing empty entry from final newline.
        if (!result.empty() && result.back().empty())
        {
            result.pop_back();
        }
        return result;
    }
};
} // namespace WSLCTestHelpers