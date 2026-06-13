/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIReporterUnitTests.cpp

Abstract:

    Unit tests for OutputChannel, OutputWriter, and Reporter.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "OutputWriter.h"
#include "Reporter.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::common::vt;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIReporterUnitTests {

// RAII pipe pair for capturing FILE* output in tests.
// write() end is passed to OutputChannel; captured() drains the read end after flush.
struct CapturePipe
{
    CapturePipe()
    {
        auto [r, w] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, false, false);
        const int writeFd = _open_osfhandle(reinterpret_cast<intptr_t>(w.get()), _O_WRONLY | _O_TEXT);
        THROW_HR_IF(E_FAIL, writeFd < 0);
        w.release();

        // Match what SetCrtEncoding(_O_U8TEXT) does in production so fwprintf works.
        WI_VERIFY(_setmode(writeFd, _O_U8TEXT) != -1);

        m_file = _fdopen(writeFd, "w");
        THROW_HR_IF(E_FAIL, m_file == nullptr);

        m_reader = std::make_unique<PartialHandleRead>(r.release());
    }

    ~CapturePipe()
    {
        if (m_file)
        {
            fclose(m_file);
        }
    }

    NON_COPYABLE(CapturePipe);
    NON_MOVABLE(CapturePipe);

    FILE* file() const
    {
        return m_file;
    }

    std::wstring captured()
    {
        if (m_file)
        {
            fclose(m_file);
            m_file = nullptr;
        }

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
    FILE* m_file = nullptr;
    std::unique_ptr<PartialHandleRead> m_reader;
};

// Reporter wired to a single capture pipe for full output capture.
// VT is disabled (not a console handle), so error output stays in the same pipe.
struct CaptureReporter
{
    CapturePipe pipe;
    Reporter reporter;

    explicit CaptureReporter(bool vtEnabled = false) : reporter(pipe.file(), vtEnabled)
    {
    }

    std::wstring captured()
    {
        return pipe.captured();
    }
};

struct SplitCaptureReporter
{
    CapturePipe outPipe;
    CapturePipe errPipe;
    Reporter reporter;

    explicit SplitCaptureReporter(bool vtEnabled = false) : reporter(outPipe.file(), vtEnabled, errPipe.file(), vtEnabled)
    {
    }
};

class WSLCCLIReporterUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIReporterUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(OutputChannel_DisableStopsWrites)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), false};

        channel.WriteString(L"before");
        channel.Disable();
        channel.WriteString(L"after");

        VERIFY_ARE_EQUAL(std::wstring{L"before"}, pipe.captured());
    }

    TEST_METHOD(OutputChannel_WriteStringIsNoOpWhenDisabled)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), false};
        channel.Disable();
        channel.WriteString(L"suppressed");
        VERIFY_ARE_EQUAL(std::wstring{L""}, pipe.captured());
    }

    TEST_METHOD(OutputChannel_SetVTEnabledTogglesIsVTEnabled)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), false};
        VERIFY_IS_FALSE(channel.IsVTEnabled());
        channel.SetVTEnabled(true);
        VERIFY_IS_TRUE(channel.IsVTEnabled());
    }

    TEST_METHOD(OutputWriter_WritesText)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), false};

        {
            OutputWriter writer{channel};
            writer << L"hello" << std::endl;
        }

        VERIFY_ARE_EQUAL(std::wstring{L"hello\n"}, pipe.captured());
    }

    TEST_METHOD(OutputWriter_AppliesFormatBeforeText)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), true};

        {
            OutputWriter writer{channel, true, true, true};
            writer.AddFormat(Format::Fg::BrightYellow);
            writer << L"warning" << std::endl;
        }

        const auto result = pipe.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"warning"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\x1b'));
    }

    TEST_METHOD(OutputWriter_SuppressesFormatWhenVTDisabled)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), false};

        {
            OutputWriter writer{channel, true, false, false};
            writer.AddFormat(Format::Fg::BrightYellow);
            writer << L"plain" << std::endl;
        }

        VERIFY_ARE_EQUAL(std::wstring{L"plain\n"}, pipe.captured());
    }

    TEST_METHOD(OutputWriter_IncomingSequenceExtendsFormatLifetime)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), true};

        {
            OutputWriter writer{channel, true, true, true};
            writer.AddFormat(Format::Default);
            writer << Format::Fg::BrightCyan;
            writer << L"text" << std::endl;
        }

        const auto result = pipe.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"text"));
        // Three ESC bytes: the incoming BrightCyan sequence, the level format (Default),
        // and the SGR reset appended by Flush().
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), std::count(result.begin(), result.end(), L'\x1b'));
    }

    TEST_METHOD(OutputWriter_ClearFormatRemovesAccumulatedFormat)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), true};

        {
            OutputWriter writer{channel, true, true, true};
            writer.AddFormat(Format::Fg::BrightRed);
            writer.ClearFormat();
            writer << L"text" << std::endl;
        }

        const auto result = pipe.captured();
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L'\x1b'));
    }

    TEST_METHOD(OutputWriter_ColorSequenceFiltering)
    {
        const std::wstring cursorUp{Cursor::Up(1).Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};
        const std::wstring reset{Format::Default.Get()};

        // Color disabled: color sequences stripped, structural sequences pass through.
        {
            CapturePipe pipe;
            OutputChannel channel{pipe.file(), true};

            {
                OutputWriter writer{channel, true, true, false};
                writer << Format::Fg::BrightRed << L"hello" << Cursor::Up(1) << L"world" << std::endl;
            }

            VERIFY_ARE_EQUAL(std::wstring{L"hello"} + cursorUp + L"world\n", pipe.captured());
        }

        // Color enabled: all sequences pass through.
        {
            CapturePipe pipe;
            OutputChannel channel{pipe.file(), true};

            {
                OutputWriter writer{channel, true, true, true};
                writer << Format::Fg::BrightRed << L"hello" << Cursor::Up(1) << L"world" << std::endl;
            }

            VERIFY_ARE_EQUAL(red + L"hello" + cursorUp + L"world" + reset + L"\n", pipe.captured());
        }
    }

    TEST_METHOD(OutputWriter_MidStreamFlushPreservesContentAndReAppliesFormat)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), true};

        {
            OutputWriter writer{channel, true, true, true};
            writer.AddFormat(Format::Fg::BrightYellow);
            writer << L"part1" << std::flush;
            writer << L"part2" << std::endl;
        }

        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring reset{Format::Default.Get()};
        VERIFY_ARE_EQUAL(yellow + L"part1" + reset + yellow + L"part2" + reset + L"\n", pipe.captured());
    }

    // Stress test: flush semantics on a long-lived writer.
    // Exercises: endl auto-flush, mid-line flush + format re-application,
    // blank-line endl with no format, redundant post-endl flush (must be a
    // no-op), and the destructor safety-net flush (must be a no-op when the
    // last manipulator already flushed). Hundreds of iterations with rotating
    // colors and variable-length text stress the reset-before-newline path
    // in Flush() and the m_formatDelay re-arming logic.
    TEST_METHOD(OutputWriter_FlushStressLongLivedWriter)
    {
        CapturePipe pipe;
        OutputChannel channel{pipe.file(), true};

        constexpr size_t iterations = 256;
        const Sequence* const palette[4] = {
            &Format::Fg::BrightCyan,
            &Format::Fg::BrightYellow,
            &Format::Fg::BrightRed,
            &Format::Default,
        };
        const std::wstring reset{Format::Default.Get()};

        std::wstring expected;
        expected.reserve(iterations * 32);

        {
            OutputWriter writer{channel, true, true, true};

            for (size_t i = 0; i < iterations; ++i)
            {
                const Sequence& color = *palette[i % 4];
                const std::wstring colorStr{color.Get()};
                const std::wstring text = L"line-" + std::to_wstring(i);

                // Reset accumulated level format so AddFormat() below doesn't
                // pile color sequences across iterations.
                writer.ClearFormat();

                switch (i % 3)
                {
                case 0:
                    // Single-segment line: endl alone must flush.
                    writer.AddFormat(color);
                    writer << text << std::endl;
                    expected.append(colorStr).append(text).append(reset).append(L"\n");
                    break;

                case 1:
                    // Mid-line flush splits the line; the level format must
                    // re-apply on the next text write, and no stray newline
                    // may appear at the split point.
                    writer.AddFormat(color);
                    writer << L"a-" << text << std::flush;
                    writer << L"-b" << std::endl;
                    expected.append(colorStr).append(L"a-").append(text).append(reset);
                    expected.append(colorStr).append(L"-b").append(reset).append(L"\n");
                    break;

                case 2:
                    // Blank line: no format, no text, just newline. Must not
                    // emit any SGR, reset, or stray bytes.
                    writer << std::endl;
                    expected.append(L"\n");
                    break;
                }

                // Redundant flush after a flushing manipulator must be a no-op:
                // m_flushed is true here, so Flush() returns early and the
                // channel sees no extra write.
                writer << std::flush;
            }
        }

        // Destructor's safety-net Flush() must be a no-op: every iteration
        // ended with endl (which flushed), so m_buffer is empty and m_flushed
        // is true at scope exit. Any extra bytes here would surface as a
        // mismatch in the equality check below.
        const auto actual = pipe.captured();
        VERIFY_ARE_EQUAL(expected, actual);
        VERIFY_ARE_EQUAL(iterations, static_cast<size_t>(std::count(actual.begin(), actual.end(), L'\n')));
    }

    TEST_METHOD(Reporter_DisabledLevel)
    {
        {
            CaptureReporter cap;
            cap.reporter.SetLevelMask(Reporter::Level::Debug, false);
            cap.reporter.Debug() << L"should not appear" << std::endl;
            VERIFY_ARE_EQUAL(std::wstring{L""}, cap.captured());
        }
        {
            CaptureReporter cap;
            cap.reporter.SetLevelMask(Reporter::Level::Debug, false);
            cap.reporter.SetLevelMask(Reporter::Level::Debug, true);
            cap.reporter.Debug() << L"re-enabled" << std::endl;
            VERIFY_ARE_EQUAL(std::wstring{L"re-enabled\n"}, cap.captured());
        }
        {
            CaptureReporter cap;
            VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Info));
            VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Error));
            cap.reporter.SetLevelMask(Reporter::Level::Info, false);
            VERIFY_IS_FALSE(cap.reporter.IsLevelEnabled(Reporter::Level::Info));
            VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Error));
        }
    }

    TEST_METHOD(Reporter_CloseOutputWriterForceDisableStopsOutput)
    {
        CaptureReporter cap;
        cap.reporter.CloseOutputWriter(true);
        cap.reporter.Info() << L"after close" << std::endl;
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.captured());
    }

    TEST_METHOD(Reporter_SetNoColorAndIsColorEnabled)
    {
        CaptureReporter cap{};

        // Default: user has not opted out.
        VERIFY_IS_TRUE(cap.reporter.IsColorEnabled());
        cap.reporter.SetNoColor(true);
        VERIFY_IS_FALSE(cap.reporter.IsColorEnabled());
        cap.reporter.SetNoColor(false);
        VERIFY_IS_TRUE(cap.reporter.IsColorEnabled());
    }

    // Interactive-console view: stdout and stderr both render to the same
    // screen buffer in emission order. This test asserts what the user
    // actually sees on their terminal across all four levels, including
    // proper SGR reset between adjacent diagnostics (no color bleed).
    TEST_METHOD(Reporter_InteractiveConsoleView)
    {
        CaptureReporter cap{true}; // single pipe, VT on — models a real TTY.
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Info() << L"starting" << std::endl;
        cap.reporter.Debug() << L"trace" << std::endl;
        cap.reporter.Warn() << L"careful" << std::endl;
        cap.reporter.Error() << L"failed" << std::endl;

        const std::wstring def{Format::Default.Get()};
        const std::wstring dim{Format::Dim.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};
        const std::wstring reset{Format::Default.Get()};

        const auto expected = def + L"starting" + reset + L"\n" + dim + L"trace" + reset + L"\n" + yellow + L"careful" + reset +
                              L"\n" + red + L"failed" + reset + L"\n";

        VERIFY_ARE_EQUAL(expected, cap.captured());
    }

    // Diagnostic levels (Debug, Warning, Error) route to stderr; non-diagnostic
    // levels (Info) route to stdout. This mirrors gcc/clang/git/etc.
    TEST_METHOD(Reporter_RoutingByLevel)
    {
        SplitCaptureReporter cap;
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Info() << L"info text" << std::endl;
        cap.reporter.Debug() << L"debug text" << std::endl;
        cap.reporter.Warn() << L"warn text" << std::endl;
        cap.reporter.Error() << L"error text" << std::endl;

        VERIFY_ARE_EQUAL(std::wstring{L"info text\n"}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L"debug text\nwarn text\nerror text\n"}, cap.errPipe.captured());
    }

    // Per-level color verification: with VT enabled on both pipes, each level
    // must emit its assigned SGR sequence into the correct stream. Mirrors
    // Reporter_RoutingByLevel but adds the color dimension that test cannot
    // cover (SplitCaptureReporter default is VT-off so SGR is suppressed).
    TEST_METHOD(Reporter_ColorByLevel)
    {
        SplitCaptureReporter cap{true};
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Info() << L"info text" << std::endl;
        cap.reporter.Debug() << L"debug text" << std::endl;
        cap.reporter.Warn() << L"warn text" << std::endl;
        cap.reporter.Error() << L"error text" << std::endl;

        const std::wstring def{Format::Default.Get()};
        const std::wstring dim{Format::Dim.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};
        const std::wstring reset{Format::Default.Get()};

        // Info is the only level that goes to stdout; its level format is Default.
        VERIFY_ARE_EQUAL(def + L"info text" + reset + L"\n", cap.outPipe.captured());

        // Diagnostics on stderr, each with its own color, concatenated in emission order.
        const auto expectedErr =
            dim + L"debug text" + reset + L"\n" + yellow + L"warn text" + reset + L"\n" + red + L"error text" + reset + L"\n";
        VERIFY_ARE_EQUAL(expectedErr, cap.errPipe.captured());
    }
};

} // namespace WSLCCLIReporterUnitTests
