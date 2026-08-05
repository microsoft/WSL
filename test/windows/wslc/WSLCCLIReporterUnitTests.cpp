/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIReporterUnitTests.cpp

Abstract:

    Unit tests for OutputChannel, InputChannel, and Reporter.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "InputChannel.h"
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

// Reporter wired with a preloaded input pipe plus split output capture, so prompt
// input and the label/newline it writes can be asserted together.
struct InputCaptureReporter
{
    CapturePipe outPipe;
    CapturePipe errPipe;
    InputPipe inPipe;
    Reporter reporter;

    explicit InputCaptureReporter(const std::wstring& input, bool interactive = false) :
        inPipe(input), reporter(outPipe.file(), false, errPipe.file(), false, inPipe.file(), interactive)
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

    TEST_METHOD(InputChannel_ReadLineReturnsNulloptAtEof)
    {
        InputPipe pipe{L""};
        const InputChannel channel{pipe.file(), false};
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLineSplitsOnNewline)
    {
        InputPipe pipe{L"user\npass\n"};
        const InputChannel channel{pipe.file(), false};

        auto first = channel.ReadLine(false);
        VERIFY_IS_TRUE(first.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L"user"}, first.value());

        auto second = channel.ReadLine(false);
        VERIFY_IS_TRUE(second.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L"pass"}, second.value());

        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLineStripsCarriageReturn)
    {
        InputPipe pipe{L"user\r\npass\r\n"};
        const InputChannel channel{pipe.file(), false};

        VERIFY_ARE_EQUAL(std::wstring{L"user"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_ARE_EQUAL(std::wstring{L"pass"}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineReturnsEmptyStringForBlankLine)
    {
        // A bare empty line is distinct from EOF: value present but empty.
        InputPipe pipe{L"\nsecond\n"};
        const InputChannel channel{pipe.file(), false};

        auto blank = channel.ReadLine(false);
        VERIFY_IS_TRUE(blank.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L""}, blank.value());

        VERIFY_ARE_EQUAL(std::wstring{L"second"}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineReturnsFinalLineWithoutTrailingNewline)
    {
        InputPipe pipe{L"only"};
        const InputChannel channel{pipe.file(), false};

        VERIFY_ARE_EQUAL(std::wstring{L"only"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_IsInteractiveReflectsOverrideForNonConsole)
    {
        InputPipe pipe{L"x\n"};
        const InputChannel notInteractive{pipe.file(), false};
        VERIFY_IS_FALSE(notInteractive.IsInteractive());

        InputPipe pipe2{L"x\n"};
        const InputChannel interactive{pipe2.file(), true};
        VERIFY_IS_TRUE(interactive.IsInteractive());
    }

    TEST_METHOD(InputChannel_ReadLineWithMaskReadsWhenNoConsole)
    {
        // Masking is a no-op without a real console; the read still succeeds.
        InputPipe pipe{L"secret\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, channel.ReadLine(true).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineOnNullFileReturnsNullopt)
    {
        const InputChannel channel{static_cast<FILE*>(nullptr), false};
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLinePreservesInteriorAndSurroundingWhitespace)
    {
        // Only the trailing CR/LF is stripped. Leading, interior, and trailing spaces
        // and tabs are preserved verbatim (WSLC does not trim, unlike Docker's prompt).
        InputPipe pipe{L"  spaced \t value  \n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"  spaced \t value  "}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineStripsLoneTrailingCarriageReturnAtEof)
    {
        // A lone trailing CR (no following LF) is not collapsed by the stream's CRLF
        // translation, so it reaches ReadLine and exercises the trailing-CR strip.
        InputPipe pipe{L"value\r"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"value"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLinePreservesEmbeddedCarriageReturn)
    {
        // Only a trailing CR is stripped; a CR in the middle of a line is preserved.
        InputPipe pipe{L"a\rb\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"a\rb"}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineDecodesUnicode)
    {
        // UTF-8 bytes on the wire decode back to the original wide characters.
        const std::wstring expected = L"\u00e9\u4e2d\u6587\u2013user";
        InputPipe pipe{expected + L"\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(expected, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineHandlesLongLine)
    {
        // Lines longer than any internal buffer are read in full (fgetwc loop).
        const std::wstring expected(8192, L'z');
        InputPipe pipe{expected + L"\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(expected, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineReturnsNulloptAfterAllLinesConsumed)
    {
        InputPipe pipe{L"one\ntwo\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"one"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_ARE_EQUAL(std::wstring{L"two"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
        // Further reads keep returning nullopt (idempotent at EOF).
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(Reporter_ReadLineReturnsInput)
    {
        InputCaptureReporter cap{L"line1\nline2\n"};
        VERIFY_ARE_EQUAL(std::wstring{L"line1"}, cap.reporter.ReadLine().value_or(L"<eof>"));
        VERIFY_ARE_EQUAL(std::wstring{L"line2"}, cap.reporter.ReadLine().value_or(L"<eof>"));
        VERIFY_IS_FALSE(cap.reporter.ReadLine().has_value());
    }

    TEST_METHOD(Reporter_IsInputInteractiveReflectsChannel)
    {
        InputCaptureReporter pipeInput{L"x\n", /*interactive*/ false};
        VERIFY_IS_FALSE(pipeInput.reporter.IsInputInteractive());

        InputCaptureReporter consoleInput{L"x\n", /*interactive*/ true};
        VERIFY_IS_TRUE(consoleInput.reporter.IsInputInteractive());
    }

    TEST_METHOD(Reporter_PromptForLineWritesLabelToStdoutAndReturnsInput)
    {
        InputCaptureReporter cap{L"myuser\n"};

        const auto result = cap.reporter.PromptForLine(Reporter::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(std::wstring{L"myuser"}, result);

        // Label lands on stdout (Docker convention); nothing on stderr; no trailing
        // newline because the input was not masked.
        VERIFY_ARE_EQUAL(std::wstring{L"Username: "}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
    }

    TEST_METHOD(Reporter_PromptForLineMaskedInteractiveEmitsTrailingNewline)
    {
        // Interactive override makes willMask true, so the un-echoed Enter is advanced
        // with a trailing newline after the label.
        InputCaptureReporter cap{L"secret\n", /*interactive*/ true};

        const auto result = cap.reporter.PromptForLine(Reporter::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: \n"}, cap.outPipe.captured());
    }

    TEST_METHOD(Reporter_PromptForLineMaskedNonInteractiveEmitsNoTrailingNewline)
    {
        // Redirected input is not interactive, so no masking and no trailing newline.
        InputCaptureReporter cap{L"secret\n", /*interactive*/ false};

        const auto result = cap.reporter.PromptForLine(Reporter::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: "}, cap.outPipe.captured());
    }

    TEST_METHOD(Reporter_PromptForLineReturnsEmptyStringAtEof)
    {
        InputCaptureReporter cap{L""};
        const auto result = cap.reporter.PromptForLine(Reporter::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(std::wstring{L""}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Username: "}, cap.outPipe.captured());
    }

    TEST_METHOD(Reporter_PromptForLineEmitsLabelVerbatimWithFormatCharacters)
    {
        // The label is passed as a formatting argument, not a format string, so brace
        // and percent characters in it must never be interpreted (no format injection).
        InputCaptureReporter cap{L"answer\n"};
        const std::wstring label = L"Value {} {0} {name} 100% ${var}: ";

        const auto result = cap.reporter.PromptForLine(Reporter::Level::Output, label, false);
        VERIFY_ARE_EQUAL(std::wstring{L"answer"}, result);
        VERIFY_ARE_EQUAL(label, cap.outPipe.captured());
    }

    TEST_METHOD(Reporter_PromptForLineDoesNotTrimPasswordWhitespace)
    {
        // Secrets are opaque: interior and surrounding whitespace is preserved so a
        // password like "  a b  " is returned exactly as typed.
        InputCaptureReporter cap{L"  a b  \n", /*interactive*/ true};

        const auto result = cap.reporter.PromptForLine(Reporter::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"  a b  "}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: \n"}, cap.outPipe.captured());
    }

    TEST_METHOD(Reporter_PromptForLineReturnsUnicodeInput)
    {
        const std::wstring expected = L"\u00fcser\u00f1ame";
        InputCaptureReporter cap{expected + L"\n"};

        const auto result = cap.reporter.PromptForLine(Reporter::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(expected, result);
    }

    TEST_METHOD(Reporter_ReadLineMaskDefaultsToUnmasked)
    {
        // ReadLine(bool mask = false): the default reads without masking and returns
        // the line, used by the --password-stdin path.
        InputCaptureReporter cap{L"piped-secret\n"};
        VERIFY_ARE_EQUAL(std::wstring{L"piped-secret"}, cap.reporter.ReadLine().value_or(L"<eof>"));
        // Nothing is written for a bare ReadLine (no prompt label).
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
    }
};

} // namespace WSLCCLIReporterUnitTests
