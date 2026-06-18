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

// Dual-pipe Reporter so stdout and stderr can be asserted independently.
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

    TEST_METHOD(Reporter_WriteEmitsExactText)
    {
        CaptureReporter cap;
        cap.reporter.Output(L"hello\n");
        VERIFY_ARE_EQUAL(std::wstring{L"hello\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_WriteWithoutNewline)
    {
        CaptureReporter cap;
        cap.reporter.Write(Reporter::Level::Output, L"hello");
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, cap.captured());
    }

    TEST_METHOD(Reporter_FormatStringSubstitutesArgs)
    {
        CaptureReporter cap;
        cap.reporter.Output(L"value={}, name={}\n", 42, L"alice");
        VERIFY_ARE_EQUAL(std::wstring{L"value=42, name=alice\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_PlainStringNeedsNoArgs)
    {
        CaptureReporter cap;
        cap.reporter.Output(L"plain literal\n");
        VERIFY_ARE_EQUAL(std::wstring{L"plain literal\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_SequenceEmittedWhenVTEnabled)
    {
        CaptureReporter cap{/*vtEnabled*/ true};
        cap.reporter.Output(L"{}highlighted{}\n", Format::Fg::BrightYellow, Format::Default);

        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"highlighted"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightYellow.Get()));
    }

    TEST_METHOD(Reporter_SequenceStrippedWhenVTDisabled)
    {
        CaptureReporter cap{/*vtEnabled*/ false};
        cap.reporter.Output(L"{}plain{}\n", Format::Fg::BrightYellow, Format::Default);
        VERIFY_ARE_EQUAL(std::wstring{L"plain\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_ColorSequenceStrippedWhenNoColor)
    {
        CaptureReporter cap{/*vtEnabled*/ true};
        cap.reporter.SetNoColor(true);

        // Color sequence (SGR) stripped; cursor moves (non-color) still pass.
        cap.reporter.Output(L"{}{}plain{}\n", Cursor::Up(1), Format::Fg::BrightRed, Format::Default);

        const auto result = cap.captured();
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightRed.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Cursor::Up(1).Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"plain"));
    }

    TEST_METHOD(Reporter_ConstructedSequenceHandledLikeSequence)
    {
        CaptureReporter cap{/*vtEnabled*/ true};
        const auto cursor = Cursor::Up(3);
        cap.reporter.Output(L"{}done\n", cursor);

        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(cursor.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"done"));
    }

    TEST_METHOD(Reporter_LevelColorWrapsOutputWhenVTEnabled)
    {
        CaptureReporter cap{/*vtEnabled*/ true};

        cap.reporter.Output(L"starting\n");
        cap.reporter.Info(L"pulling\n");
        cap.reporter.Warn(L"careful\n");
        cap.reporter.Error(L"failed\n");

        const std::wstring def{Format::Default.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};

        const auto expected = std::wstring{L"starting\npulling\n"} + yellow + L"careful\n" + def + red + L"failed\n" + def;

        VERIFY_ARE_EQUAL(expected, cap.captured());
    }

    TEST_METHOD(Reporter_LevelColorSuppressedWhenVTDisabled)
    {
        CaptureReporter cap{/*vtEnabled*/ false};
        cap.reporter.Error(L"failed\n");
        VERIFY_ARE_EQUAL(std::wstring{L"failed\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_LevelColorSuppressedWhenNoColor)
    {
        CaptureReporter cap{/*vtEnabled*/ true};
        cap.reporter.SetNoColor(true);
        cap.reporter.Warn(L"careful\n");
        VERIFY_ARE_EQUAL(std::wstring{L"careful\n"}, cap.captured());
    }

    TEST_METHOD(Reporter_RoutingByLevel)
    {
        SplitCaptureReporter cap;

        cap.reporter.Output(L"output text\n");
        cap.reporter.Info(L"info text\n");
        cap.reporter.Warn(L"warn text\n");
        cap.reporter.Error(L"error text\n");

        VERIFY_ARE_EQUAL(std::wstring{L"output text\n"}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L"info text\nwarn text\nerror text\n"}, cap.errPipe.captured());
    }

    TEST_METHOD(Reporter_SetNoColorTogglesIsNoColor)
    {
        CaptureReporter cap;
        VERIFY_IS_FALSE(cap.reporter.IsNoColor());
        cap.reporter.SetNoColor(true);
        VERIFY_IS_TRUE(cap.reporter.IsNoColor());
        cap.reporter.SetNoColor(false);
        VERIFY_IS_FALSE(cap.reporter.IsNoColor());
    }

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
            CapturePipe outPipe;
            CapturePipe errPipe;
            Reporter reporter{outPipe.file(), /*outVt*/ true, errPipe.file(), /*errVt*/ false};
            VERIFY_IS_TRUE(reporter.IsVTEnabled(Reporter::Level::Output));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Info));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Warning));
            VERIFY_IS_FALSE(reporter.IsVTEnabled(Reporter::Level::Error));
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
    }

    TEST_METHOD(Reporter_Write_MixesSequencesWithStandardFormatArgs)
    {
        // Reporter.Write is std::format under the hood — any formattable type works
        // alongside Sequences. Sequences are stripped when color is off; everything
        // else formats normally through std::format machinery.
        //
        // This test exercises four sequence categories in a single format call:
        //   SGR color   (Format::Fg::BrightRed)  — color, stripped by NoColor
        //   Non-color   (Erase::LineForward)      — not color, survives NoColor
        //   Hyperlink   (ConstructedSequence OSC8) — color, stripped by NoColor
        //   SGR reset   (Format::Default)         — color, stripped by NoColor
        //
        // Hyperlink open/close are separate sequences so the visible link text
        // degrades gracefully when sequences are stripped.

        const auto& eraseLine = Erase::LineForward; // \x1b[K  — non-color CSI
        const auto linkOpen = Format::LinkOpen(L"https://example.com");
        const auto& linkClose = Format::LinkClose;

        // Format: <color>Count: <int>, hex: <hex>, <erase><linkOpen>click here<linkClose><reset>
        constexpr auto fmt = L"{}Count: {}, hex: {:04x}, {}{}click here{}{}\n";

        // VT + color enabled: equivalent to std::format with all sequence bytes.
        {
            CaptureReporter cap{/*vtEnabled*/ true};
            cap.reporter.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const auto expected = std::format(
                fmt, Format::Fg::BrightRed.Get(), 42, 255u, eraseLine.Get(), linkOpen.Get(), linkClose.Get(), Format::Default.Get());
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }

        // NoColor (VT enabled, color disabled): non-color sequences pass through,
        // color sequences (SGR, hyperlink) replaced with empty string.
        {
            CaptureReporter cap{/*vtEnabled*/ true};
            cap.reporter.SetNoColor(true);
            cap.reporter.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const std::wstring_view empty;
            const auto expected = std::format(fmt, empty, 42, 255u, eraseLine.Get(), empty, empty, empty);
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }

        // VT disabled: all sequences replaced with empty string.
        {
            CaptureReporter cap{/*vtEnabled*/ false};
            cap.reporter.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const std::wstring_view empty;
            const auto expected = std::format(fmt, empty, 42, 255u, empty, empty, empty, empty);
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }
    }
};

} // namespace WSLCCLIReporterUnitTests
