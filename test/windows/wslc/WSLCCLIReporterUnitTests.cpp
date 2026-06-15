/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIReporterUnitTests.cpp

Abstract:

    Unit tests for OutputChannel and Reporter.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "OutputChannel.h"
#include "Reporter.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::common::vt;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIReporterUnitTests {

// Reporter wired to two separate capture pipes so stdout and stderr can be
// asserted independently. VT defaults to off to model a redirected pipe.
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

    // -------------------------------------------------------------------------
    // OutputChannel: raw byte-sink behavior.
    // -------------------------------------------------------------------------

    TEST_METHOD(OutputChannel_WriteStringWritesText)
    {
        CapturePipe pipe;
        const OutputChannel channel{pipe.file(), false};
        channel.WriteString(L"hello");
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, pipe.captured());
    }

    TEST_METHOD(OutputChannel_WriteStringIsNoOpOnEmpty)
    {
        CapturePipe pipe;
        const OutputChannel channel{pipe.file(), false};
        channel.WriteString(L"");
        VERIFY_ARE_EQUAL(std::wstring{L""}, pipe.captured());
    }

    TEST_METHOD(OutputChannel_FromHandleFallsBackToFileForNonConsole)
    {
        CapturePipe pipe;
        const OutputChannel channel{INVALID_HANDLE_VALUE, pipe.file()};
        // Non-console handle => uses the file path; writes succeed via fwprintf.
        channel.WriteString(L"fallback");
        VERIFY_ARE_EQUAL(std::wstring{L"fallback"}, pipe.captured());
        VERIFY_IS_FALSE(channel.GetConsoleWidth().has_value());
    }

    TEST_METHOD(OutputChannel_GetConsoleWidth_FileChannelReturnsNullopt)
    {
        CapturePipe pipe;
        const OutputChannel channel{pipe.file(), false};
        VERIFY_IS_FALSE(channel.GetConsoleWidth().has_value());
    }

    // -------------------------------------------------------------------------
    // Reporter: WriteLine / Write text output.
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_WriteLineAppendsNewline)
    {
        CaptureReporter cap;
        cap.reporter.Output(L"hello");
        VERIFY_ARE_EQUAL(std::wstring{L"hello\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_WriteSuppressesNewline)
    {
        CaptureReporter cap;
        cap.reporter.Write(Reporter::Level::Output, L"hello");
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, cap.captured());
    }

    TEST_METHOD(Reporter_FormatStringSubstitutesArgs)
    {
        CaptureReporter cap;
        cap.reporter.Output(L"value={}, name={}", 42, L"alice");
        VERIFY_ARE_EQUAL(std::wstring{L"value=42, name=alice\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_PlainStringNeedsNoArgs)
    {
        CaptureReporter cap;
        cap.reporter.Output(L"plain literal");
        VERIFY_ARE_EQUAL(std::wstring{L"plain literal\n"}, cap.captured());
    }

    // -------------------------------------------------------------------------
    // Reporter: Sequence handling. The user-visible contract is that
    // Sequence arguments embedded in the format string are emitted as VT bytes
    // when VT is enabled, and stripped to empty when VT is off (or, for color
    // sequences, when --no-color is set).
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_SequenceEmittedWhenVTEnabled)
    {
        CaptureReporter cap{/*vtEnabled*/ true};
        cap.reporter.Output(L"{}highlighted{}", Format::Fg::BrightYellow, Format::Default);

        // The captured output should contain the BrightYellow SGR bytes.
        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"highlighted"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightYellow.Get()));
    }

    TEST_METHOD(Reporter_SequenceStrippedWhenVTDisabled)
    {
        CaptureReporter cap{/*vtEnabled*/ false};
        cap.reporter.Output(L"{}plain{}", Format::Fg::BrightYellow, Format::Default);

        // With VT off, all Sequence args expand to empty. Format-string text
        // is preserved; only the substituted bytes are removed.
        VERIFY_ARE_EQUAL(std::wstring{L"plain\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_ColorSequenceStrippedWhenNoColor)
    {
        CaptureReporter cap{/*vtEnabled*/ true};
        cap.reporter.SetNoColor(true);

        // Color sequence (SGR) stripped; cursor moves (non-color) still pass.
        cap.reporter.Output(L"{}{}plain{}", Cursor::Up(1), Format::Fg::BrightRed, Format::Default);

        const auto result = cap.captured();
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightRed.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Cursor::Up(1).Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"plain"));
    }

    TEST_METHOD(Reporter_ConstructedSequenceHandledLikeSequence)
    {
        CaptureReporter cap{/*vtEnabled*/ true};
        const auto cursor = Cursor::Up(3);
        cap.reporter.Output(L"{}done", cursor);

        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(cursor.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"done"));
    }

    // -------------------------------------------------------------------------
    // Reporter: level color prefix/reset sandwich.
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_LevelColorWrapsOutputWhenVTEnabled)
    {
        // Single pipe + VT on models a real TTY where stdout and stderr render
        // to the same buffer.
        CaptureReporter cap{/*vtEnabled*/ true};
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Output(L"starting");
        cap.reporter.Debug(L"trace");
        cap.reporter.Info(L"pulling");
        cap.reporter.Warn(L"careful");
        cap.reporter.Error(L"failed");

        const std::wstring def{Format::Default.Get()};
        const std::wstring dim{Format::Dim.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};

        const auto expected = def + L"starting" + def + L"\n" + dim + L"trace" + def + L"\n" + def + L"pulling" + def + L"\n" +
                              yellow + L"careful" + def + L"\n" + red + L"failed" + def + L"\n";

        VERIFY_ARE_EQUAL(expected, cap.captured());
    }

    TEST_METHOD(Reporter_LevelColorSuppressedWhenVTDisabled)
    {
        // VT off => no SGR prefix and no reset are emitted; body only.
        CaptureReporter cap{/*vtEnabled*/ false};
        cap.reporter.Error(L"failed");
        VERIFY_ARE_EQUAL(std::wstring{L"failed\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_LevelColorSuppressedWhenNoColor)
    {
        // VT on, but --no-color set: skip SGR sandwich; structural VT (cursor
        // moves) inside the body would still pass via Sequence args, but the
        // level wrapper colors are unconditionally suppressed.
        CaptureReporter cap{/*vtEnabled*/ true};
        cap.reporter.SetNoColor(true);
        cap.reporter.Warn(L"careful");
        VERIFY_ARE_EQUAL(std::wstring{L"careful\n"}, cap.captured());
    }

    // -------------------------------------------------------------------------
    // Reporter: routing by level. Output -> stdout; everything else -> stderr.
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_RoutingByLevel)
    {
        SplitCaptureReporter cap;
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);

        cap.reporter.Output(L"output text");
        cap.reporter.Debug(L"debug text");
        cap.reporter.Info(L"info text");
        cap.reporter.Warn(L"warn text");
        cap.reporter.Error(L"error text");

        VERIFY_ARE_EQUAL(std::wstring{L"output text\n"}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L"debug text\ninfo text\nwarn text\nerror text\n"}, cap.errPipe.captured());
    }

    // -------------------------------------------------------------------------
    // Reporter: level mask + Disable.
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_DisabledLevelProducesNoOutput)
    {
        CaptureReporter cap;
        cap.reporter.SetLevelMask(Reporter::Level::Debug, false);
        cap.reporter.Debug(L"should not appear");
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.captured());
    }

    TEST_METHOD(Reporter_LevelMaskCanBeReEnabled)
    {
        CaptureReporter cap;
        cap.reporter.SetLevelMask(Reporter::Level::Debug, false);
        cap.reporter.Debug(L"suppressed");
        cap.reporter.SetLevelMask(Reporter::Level::Debug, true);
        cap.reporter.Debug(L"re-enabled");
        VERIFY_ARE_EQUAL(std::wstring{L"re-enabled\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_IsLevelEnabledReflectsMask)
    {
        CaptureReporter cap;
        VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Output));
        VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Error));
        VERIFY_IS_FALSE(cap.reporter.IsLevelEnabled(Reporter::Level::Debug));

        cap.reporter.SetLevelMask(Reporter::Level::Output, false);
        VERIFY_IS_FALSE(cap.reporter.IsLevelEnabled(Reporter::Level::Output));
        VERIFY_IS_TRUE(cap.reporter.IsLevelEnabled(Reporter::Level::Error));
    }

    TEST_METHOD(Reporter_DisableSilencesAllFurtherWrites)
    {
        CaptureReporter cap;
        cap.reporter.Output(L"before");
        cap.reporter.Disable();
        cap.reporter.Output(L"after");
        cap.reporter.Error(L"and error");
        VERIFY_ARE_EQUAL(std::wstring{L"before\n"}, cap.captured());
    }

    // -------------------------------------------------------------------------
    // Reporter: no-color toggle.
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_SetNoColorTogglesIsColorEnabled)
    {
        CaptureReporter cap;
        VERIFY_IS_TRUE(cap.reporter.IsColorEnabled());
        cap.reporter.SetNoColor(true);
        VERIFY_IS_FALSE(cap.reporter.IsColorEnabled());
        cap.reporter.SetNoColor(false);
        VERIFY_IS_TRUE(cap.reporter.IsColorEnabled());
    }

    // -------------------------------------------------------------------------
    // Reporter: per-level IsVTEnabled / IsColorEnabled.
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_IsVTEnabledReflectsPerChannelState)
    {
        {
            SplitCaptureReporter cap{/*vt*/ false};
            VERIFY_IS_FALSE(cap.reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_FALSE(cap.reporter.IsVTEnabled(Reporter::Level::Error));
        }
        {
            SplitCaptureReporter cap{/*vt*/ true};
            VERIFY_IS_TRUE(cap.reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_TRUE(cap.reporter.IsVTEnabled(Reporter::Level::Error));
        }
        {
            // Asymmetric: stdout on a real terminal, stderr redirected to a log.
            CapturePipe outPipe;
            CapturePipe errPipe;
            Reporter reporter{outPipe.file(), /*outVt*/ true, errPipe.file(), /*errVt*/ false};
            VERIFY_IS_TRUE(reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Info));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Warning));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Error));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Debug));
        }
    }

    TEST_METHOD(Reporter_IsColorEnabledPerLevelHonorsBothVTAndNoColor)
    {
        SplitCaptureReporter cap{/*vt*/ true};
        VERIFY_IS_TRUE(cap.reporter.IsColorEnabled(Reporter::Level::Output));
        VERIFY_IS_TRUE(cap.reporter.IsColorEnabled(Reporter::Level::Error));

        cap.reporter.SetNoColor(true);
        VERIFY_IS_FALSE(cap.reporter.IsColorEnabled(Reporter::Level::Output));
        VERIFY_IS_FALSE(cap.reporter.IsColorEnabled(Reporter::Level::Error));
    }

    TEST_METHOD(Reporter_GetConsoleWidthReturnsNulloptForFileChannels)
    {
        SplitCaptureReporter cap;
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Output).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Info).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Warning).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Error).has_value());
        VERIFY_IS_FALSE(cap.reporter.GetConsoleWidth(Reporter::Level::Debug).has_value());
    }

    // -------------------------------------------------------------------------
    // Reporter: thread-safety smoke test. Concurrent WriteLine calls must
    // produce intact lines (no mid-line byte interleaving) on the destination,
    // since each call is one underlying WriteConsoleW or fwprintf+fflush.
    // -------------------------------------------------------------------------

    TEST_METHOD(Reporter_ConcurrentWritesProduceIntactLines)
    {
        CaptureReporter cap;

        constexpr size_t threadCount = 4;
        constexpr size_t perThread = 64;

        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([&, t] {
                for (size_t i = 0; i < perThread; ++i)
                {
                    cap.reporter.Output(L"thread-{}-line-{}", t, i);
                }
            });
        }
        for (auto& th : threads)
        {
            th.join();
        }

        const auto captured = cap.captured();

        // Count lines and verify each begins with "thread-".
        size_t lineCount = 0;
        size_t pos = 0;
        while (pos < captured.size())
        {
            const auto nl = captured.find(L'\n', pos);
            const auto end = (nl == std::wstring::npos) ? captured.size() : nl;
            const auto line = captured.substr(pos, end - pos);
            VERIFY_IS_TRUE(line.rfind(L"thread-", 0) == 0, L"every emitted line must begin with 'thread-'");
            ++lineCount;
            pos = end + 1;
            if (nl == std::wstring::npos)
            {
                break;
            }
        }
        VERIFY_ARE_EQUAL(threadCount * perThread, lineCount);
    }
};

} // namespace WSLCCLIReporterUnitTests
