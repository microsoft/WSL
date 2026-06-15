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
            VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Output));
            VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Error));
            cap.reporter.SetLevelMask(Reporter::Level::Output, false);
            VERIFY_IS_FALSE(cap.reporter.IsLevelEnabled(Reporter::Level::Output));
            VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Error));
        }
    }

    TEST_METHOD(Reporter_CloseOutputWriterForceDisableStopsOutput)
    {
        CaptureReporter cap;
        cap.reporter.CloseOutputWriter(true);
        cap.reporter.Output() << L"after close" << std::endl;
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
    // actually sees on their terminal across all five levels, including
    // proper SGR reset between adjacent diagnostics (no color bleed).
    TEST_METHOD(Reporter_InteractiveConsoleView)
    {
        CaptureReporter cap{true}; // single pipe, VT on — models a real TTY.
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Output() << L"starting" << std::endl;
        cap.reporter.Debug() << L"trace" << std::endl;
        cap.reporter.Info() << L"pulling" << std::endl;
        cap.reporter.Warn() << L"careful" << std::endl;
        cap.reporter.Error() << L"failed" << std::endl;

        const std::wstring def{Format::Default.Get()};
        const std::wstring dim{Format::Dim.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};
        const std::wstring reset{Format::Default.Get()};

        // Info emits no SGR prefix and no trailing reset (color was never written),
        // so it appears as plain text between the colored Debug and Warn lines.
        const auto expected = def + L"starting" + reset + L"\n" + dim + L"trace" + reset + L"\n" + def + L"pulling" + reset +
                              L"\n" + yellow + L"careful" + reset + L"\n" + red + L"failed" + reset + L"\n";

        VERIFY_ARE_EQUAL(expected, cap.captured());
    }

    // Diagnostic levels (Debug, Info, Warning, Error) route to stderr; primary
    // data output (Output) routes to stdout. This mirrors gcc/clang/git/docker/etc.
    TEST_METHOD(Reporter_RoutingByLevel)
    {
        SplitCaptureReporter cap;
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Output() << L"output text" << std::endl;
        cap.reporter.Debug() << L"debug text" << std::endl;
        cap.reporter.Info() << L"info text" << std::endl;
        cap.reporter.Warn() << L"warn text" << std::endl;
        cap.reporter.Error() << L"error text" << std::endl;

        VERIFY_ARE_EQUAL(std::wstring{L"output text\n"}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L"debug text\ninfo text\nwarn text\nerror text\n"}, cap.errPipe.captured());
    }

    // Per-level color verification: with VT enabled on both pipes, each level
    // must emit its assigned SGR sequence into the correct stream. Mirrors
    // Reporter_RoutingByLevel but adds the color dimension that test cannot
    // cover (SplitCaptureReporter default is VT-off so SGR is suppressed).
    TEST_METHOD(Reporter_ColorByLevel)
    {
        SplitCaptureReporter cap{true};
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Output() << L"output text" << std::endl;
        cap.reporter.Debug() << L"debug text" << std::endl;
        cap.reporter.Info() << L"info text" << std::endl;
        cap.reporter.Warn() << L"warn text" << std::endl;
        cap.reporter.Error() << L"error text" << std::endl;

        const std::wstring def{Format::Default.Get()};
        const std::wstring dim{Format::Dim.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};
        const std::wstring reset{Format::Default.Get()};

        // Output is the only level that goes to stdout; its level format is Default.
        VERIFY_ARE_EQUAL(def + L"output text" + reset + L"\n", cap.outPipe.captured());

        // Diagnostics on stderr in emission order. Info emits no SGR prefix
        // (and no trailing reset since color was never written), so it appears
        // as plain text between the colored Debug and Warn lines.
        const auto expectedErr = dim + L"debug text" + reset + L"\n" + def + L"info text" + reset + L"\n" + yellow +
                                 L"warn text" + reset + L"\n" + red + L"error text" + reset + L"\n";
        VERIFY_ARE_EQUAL(expectedErr, cap.errPipe.captured());
    }

    // Info is informational stderr (progress, "[detached]", "Created session",
    // etc.). It must be enabled by default (so callers don't have to opt in)
    // and respect SetLevelMask like any other level.
    TEST_METHOD(Reporter_Info_EnabledByDefaultAndMaskable)
    {
        CaptureReporter cap;
        VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Info));

        cap.reporter.Info() << L"first" << std::endl;
        cap.reporter.SetLevelMask(Reporter::Level::Info, false);
        VERIFY_IS_FALSE(cap.reporter.IsLevelEnabled(Reporter::Level::Info));
        cap.reporter.Info() << L"suppressed" << std::endl;
        cap.reporter.SetLevelMask(Reporter::Level::Info, true);
        cap.reporter.Info() << L"third" << std::endl;

        VERIFY_ARE_EQUAL(std::wstring{L"first\nthird\n"}, cap.captured());
    }

    // Reporter::IsVTEnabled(Level) must reflect the per-channel VT state.
    // Output reads the out channel; Info/Warn/Error/Debug read the err channel.
    TEST_METHOD(Reporter_IsVTEnabled_PerLevelRouting)
    {
        // Symmetric off / symmetric on.
        {
            SplitCaptureReporter cap{false};
            VERIFY_IS_FALSE(cap.reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_FALSE(cap.reporter.IsVTEnabled(Reporter::Level::Info));
            VERIFY_IS_FALSE(cap.reporter.IsVTEnabled(Reporter::Level::Warning));
            VERIFY_IS_FALSE(cap.reporter.IsVTEnabled(Reporter::Level::Error));
            VERIFY_IS_FALSE(cap.reporter.IsVTEnabled(Reporter::Level::Debug));
        }
        {
            SplitCaptureReporter cap{true};
            VERIFY_IS_TRUE(cap.reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_TRUE(cap.reporter.IsVTEnabled(Reporter::Level::Info));
            VERIFY_IS_TRUE(cap.reporter.IsVTEnabled(Reporter::Level::Warning));
            VERIFY_IS_TRUE(cap.reporter.IsVTEnabled(Reporter::Level::Error));
            VERIFY_IS_TRUE(cap.reporter.IsVTEnabled(Reporter::Level::Debug));
        }

        // Asymmetric: out VT on, err VT off — the per-level dispatch must read
        // from the correct channel. Models `wslc image pull 2>logfile` where
        // stdout stays interactive but stderr is redirected.
        {
            CapturePipe outPipe;
            CapturePipe errPipe;
            Reporter reporter{outPipe.file(), true, errPipe.file(), false};

            VERIFY_IS_TRUE(reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Info));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Warning));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Error));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Debug));
        }

        // Reverse asymmetry: err interactive, out redirected (e.g. `wslc image pull >out.tar`).
        {
            CapturePipe outPipe;
            CapturePipe errPipe;
            Reporter reporter{outPipe.file(), false, errPipe.file(), true};

            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_TRUE(reporter.IsVTEnabled(Reporter::Level::Info));
            VERIFY_IS_TRUE(reporter.IsVTEnabled(Reporter::Level::Warning));
            VERIFY_IS_TRUE(reporter.IsVTEnabled(Reporter::Level::Error));
            VERIFY_IS_TRUE(reporter.IsVTEnabled(Reporter::Level::Debug));
        }
    }

    // GetConsoleWidth returns std::nullopt on any non-console destination,
    // including FILE* (redirected) and explicit handle-based channels whose
    // handle is not a console.
    TEST_METHOD(OutputChannel_GetConsoleWidth_FileChannelReturnsNullopt)
    {
        CapturePipe pipe;
        OutputChannel fileChannel{pipe.file(), false};
        VERIFY_IS_FALSE(fileChannel.GetConsoleWidth().has_value());

        // VT-enabled FILE* is still a FILE*, not a console: width must remain nullopt.
        OutputChannel fileChannelVt{pipe.file(), true};
        VERIFY_IS_FALSE(fileChannelVt.GetConsoleWidth().has_value());
    }

    TEST_METHOD(OutputChannel_GetConsoleWidth_HandleChannelWithoutConsoleReturnsNullopt)
    {
        // Handle-based ctor falls back to the FILE* when GetConsoleMode fails;
        // GetConsoleWidth must report nullopt in that case (no console buffer to query).
        CapturePipe pipe;
        OutputChannel channel{INVALID_HANDLE_VALUE, pipe.file(), false};
        VERIFY_IS_FALSE(channel.GetConsoleWidth().has_value());
    }

    // Reporter::GetConsoleWidth(Level) must delegate to the right per-level
    // channel, and propagate nullopt when the destination is a FILE*.
    TEST_METHOD(Reporter_GetConsoleWidth_PerLevelRoutingReturnsNulloptForFileChannels)
    {
        SplitCaptureReporter cap;
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Output).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Info).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Warning).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Error).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Debug).has_value());
    }
};

} // namespace WSLCCLIReporterUnitTests
