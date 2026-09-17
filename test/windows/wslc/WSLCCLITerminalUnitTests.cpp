/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLITerminalUnitTests.cpp

Abstract:

    Unit tests for OutputChannel, InputChannel, and Terminal.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "InputChannel.h"
#include "OutputChannel.h"
#include "Terminal.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::common::vt;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLITerminalUnitTests {

// Dual-pipe Terminal so stdout and stderr can be asserted independently.
struct SplitCaptureTerminal
{
    CapturePipe outPipe;
    CapturePipe errPipe;
    Terminal terminal;

    explicit SplitCaptureTerminal(bool vtEnabled = false) : terminal(outPipe.file(), vtEnabled, errPipe.file(), vtEnabled)
    {
    }
};

// Terminal wired with a preloaded input pipe plus split output capture, so prompt
// input and the label/newline it writes can be asserted together.
struct InputCaptureTerminal
{
    CapturePipe outPipe;
    CapturePipe errPipe;
    InputPipe inPipe;
    Terminal terminal;

    explicit InputCaptureTerminal(const std::wstring& input, bool interactive = false) :
        inPipe(input), terminal(outPipe.file(), false, errPipe.file(), false, inPipe.file(), interactive)
    {
    }
};

class WSLCCLITerminalUnitTests
{
    WSLC_TEST_CLASS(WSLCCLITerminalUnitTests)

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

    TEST_METHOD(Terminal_WriteEmitsExactText)
    {
        CaptureTerminal cap;
        cap.terminal.Output(L"hello\n");
        VERIFY_ARE_EQUAL(std::wstring{L"hello\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_WriteWithoutNewline)
    {
        CaptureTerminal cap;
        cap.terminal.Write(Terminal::Level::Output, L"hello");
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, cap.captured());
    }

    TEST_METHOD(Terminal_FormatStringSubstitutesArgs)
    {
        CaptureTerminal cap;
        cap.terminal.Output(L"value={}, name={}\n", 42, L"alice");
        VERIFY_ARE_EQUAL(std::wstring{L"value=42, name=alice\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_PlainStringNeedsNoArgs)
    {
        CaptureTerminal cap;
        cap.terminal.Output(L"plain literal\n");
        VERIFY_ARE_EQUAL(std::wstring{L"plain literal\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_SequenceEmittedWhenVTEnabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        cap.terminal.Output(L"{}highlighted{}\n", Format::Fg::BrightYellow, Format::Default);

        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"highlighted"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightYellow.Get()));
    }

    TEST_METHOD(Terminal_SequenceStrippedWhenVTDisabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ false};
        cap.terminal.Output(L"{}plain{}\n", Format::Fg::BrightYellow, Format::Default);
        VERIFY_ARE_EQUAL(std::wstring{L"plain\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_ColorSequenceStrippedWhenNoColor)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        cap.terminal.SetNoColor(true);

        // Color sequence (SGR) stripped; cursor moves (non-color) still pass.
        cap.terminal.Output(L"{}{}plain{}\n", Cursor::Up(1), Format::Fg::BrightRed, Format::Default);

        const auto result = cap.captured();
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightRed.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Cursor::Up(1).Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"plain"));
    }

    TEST_METHOD(Terminal_ConstructedSequenceHandledLikeSequence)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        const auto cursor = Cursor::Up(3);
        cap.terminal.Output(L"{}done\n", cursor);

        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(cursor.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"done"));
    }

    TEST_METHOD(Terminal_LevelColorWrapsOutputWhenVTEnabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};

        cap.terminal.Output(L"starting\n");
        cap.terminal.Info(L"pulling\n");
        cap.terminal.Warn(L"careful\n");
        cap.terminal.Error(L"failed\n");

        const std::wstring def{Format::Default.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};

        const auto expected = std::wstring{L"starting\npulling\n"} + yellow + L"careful\n" + def + red + L"failed\n" + def;

        VERIFY_ARE_EQUAL(expected, cap.captured());
    }

    TEST_METHOD(Terminal_LevelColorSuppressedWhenVTDisabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ false};
        cap.terminal.Error(L"failed\n");
        VERIFY_ARE_EQUAL(std::wstring{L"failed\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_LevelColorSuppressedWhenNoColor)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        cap.terminal.SetNoColor(true);
        cap.terminal.Warn(L"careful\n");
        VERIFY_ARE_EQUAL(std::wstring{L"careful\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_RoutingByLevel)
    {
        SplitCaptureTerminal cap;

        cap.terminal.Output(L"output text\n");
        cap.terminal.Info(L"info text\n");
        cap.terminal.Warn(L"warn text\n");
        cap.terminal.Error(L"error text\n");

        VERIFY_ARE_EQUAL(std::wstring{L"output text\n"}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L"info text\nwarn text\nerror text\n"}, cap.errPipe.captured());
    }

    TEST_METHOD(Terminal_SetNoColorTogglesIsNoColor)
    {
        CaptureTerminal cap;
        VERIFY_IS_FALSE(cap.terminal.IsNoColor());
        cap.terminal.SetNoColor(true);
        VERIFY_IS_TRUE(cap.terminal.IsNoColor());
        cap.terminal.SetNoColor(false);
        VERIFY_IS_FALSE(cap.terminal.IsNoColor());
    }

    TEST_METHOD(Terminal_IsVTEnabledReflectsPerChannelState)
    {
        {
            SplitCaptureTerminal cap{/*vt*/ false};
            VERIFY_IS_FALSE(cap.terminal.IsVTEnabled(Terminal::Level::Output));
            VERIFY_IS_FALSE(cap.terminal.IsVTEnabled(Terminal::Level::Error));
        }
        {
            SplitCaptureTerminal cap{/*vt*/ true};
            VERIFY_IS_TRUE(cap.terminal.IsVTEnabled(Terminal::Level::Output));
            VERIFY_IS_TRUE(cap.terminal.IsVTEnabled(Terminal::Level::Error));
        }
        {
            CapturePipe outPipe;
            CapturePipe errPipe;
            Terminal terminal{outPipe.file(), /*outVt*/ true, errPipe.file(), /*errVt*/ false};
            VERIFY_IS_TRUE(terminal.IsVTEnabled(Terminal::Level::Output));
            VERIFY_IS_FALSE(terminal.IsVTEnabled(Terminal::Level::Info));
            VERIFY_IS_FALSE(terminal.IsVTEnabled(Terminal::Level::Warning));
            VERIFY_IS_FALSE(terminal.IsVTEnabled(Terminal::Level::Error));
        }
    }

    TEST_METHOD(Terminal_IsColorEnabledPerLevelHonorsBothVTAndNoColor)
    {
        SplitCaptureTerminal cap{/*vt*/ true};
        VERIFY_IS_TRUE(cap.terminal.IsColorEnabled(Terminal::Level::Output));
        VERIFY_IS_TRUE(cap.terminal.IsColorEnabled(Terminal::Level::Error));

        cap.terminal.SetNoColor(true);
        VERIFY_IS_FALSE(cap.terminal.IsColorEnabled(Terminal::Level::Output));
        VERIFY_IS_FALSE(cap.terminal.IsColorEnabled(Terminal::Level::Error));
    }

    TEST_METHOD(Terminal_GetConsoleWidthReturnsNulloptForFileChannels)
    {
        SplitCaptureTerminal cap;
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Output).has_value());
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Info).has_value());
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Warning).has_value());
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Error).has_value());
    }

    TEST_METHOD(Terminal_Write_MixesSequencesWithStandardFormatArgs)
    {
        // Terminal.Write is std::format under the hood — any formattable type works
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
            CaptureTerminal cap{/*vtEnabled*/ true};
            cap.terminal.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const auto expected = std::format(
                fmt, Format::Fg::BrightRed.Get(), 42, 255u, eraseLine.Get(), linkOpen.Get(), linkClose.Get(), Format::Default.Get());
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }

        // NoColor (VT enabled, color disabled): non-color sequences pass through,
        // color sequences (SGR, hyperlink) replaced with empty string.
        {
            CaptureTerminal cap{/*vtEnabled*/ true};
            cap.terminal.SetNoColor(true);
            cap.terminal.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const std::wstring_view empty;
            const auto expected = std::format(fmt, empty, 42, 255u, eraseLine.Get(), empty, empty, empty);
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }

        // VT disabled: all sequences replaced with empty string.
        {
            CaptureTerminal cap{/*vtEnabled*/ false};
            cap.terminal.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

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

    TEST_METHOD(Terminal_ReadLineReturnsInput)
    {
        InputCaptureTerminal cap{L"line1\nline2\n"};
        VERIFY_ARE_EQUAL(std::wstring{L"line1"}, cap.terminal.ReadLine().value_or(L"<eof>"));
        VERIFY_ARE_EQUAL(std::wstring{L"line2"}, cap.terminal.ReadLine().value_or(L"<eof>"));
        VERIFY_IS_FALSE(cap.terminal.ReadLine().has_value());
    }

    TEST_METHOD(Terminal_IsInputInteractiveReflectsChannel)
    {
        InputCaptureTerminal pipeInput{L"x\n", /*interactive*/ false};
        VERIFY_IS_FALSE(pipeInput.terminal.IsInputInteractive());

        InputCaptureTerminal consoleInput{L"x\n", /*interactive*/ true};
        VERIFY_IS_TRUE(consoleInput.terminal.IsInputInteractive());
    }

    TEST_METHOD(Terminal_PromptForLineWritesLabelToStdoutAndReturnsInput)
    {
        InputCaptureTerminal cap{L"myuser\n"};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(std::wstring{L"myuser"}, result);

        // Label lands on stdout (Docker convention); nothing on stderr; no trailing
        // newline because the input was not masked.
        VERIFY_ARE_EQUAL(std::wstring{L"Username: "}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineMaskedInteractiveEmitsTrailingNewline)
    {
        // Interactive override makes willMask true, so the un-echoed Enter is advanced
        // with a trailing newline after the label.
        InputCaptureTerminal cap{L"secret\n", /*interactive*/ true};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: \n"}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineMaskedNonInteractiveEmitsNoTrailingNewline)
    {
        // Redirected input is not interactive, so no masking and no trailing newline.
        InputCaptureTerminal cap{L"secret\n", /*interactive*/ false};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: "}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineReturnsEmptyStringAtEof)
    {
        InputCaptureTerminal cap{L""};
        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(std::wstring{L""}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Username: "}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineEmitsLabelVerbatimWithFormatCharacters)
    {
        // The label is passed as a formatting argument, not a format string, so brace
        // and percent characters in it must never be interpreted (no format injection).
        InputCaptureTerminal cap{L"answer\n"};
        const std::wstring label = L"Value {} {0} {name} 100% ${var}: ";

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, label, false);
        VERIFY_ARE_EQUAL(std::wstring{L"answer"}, result);
        VERIFY_ARE_EQUAL(label, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineDoesNotTrimPasswordWhitespace)
    {
        // Secrets are opaque: interior and surrounding whitespace is preserved so a
        // password like "  a b  " is returned exactly as typed.
        InputCaptureTerminal cap{L"  a b  \n", /*interactive*/ true};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"  a b  "}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: \n"}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineReturnsUnicodeInput)
    {
        const std::wstring expected = L"\u00fcser\u00f1ame";
        InputCaptureTerminal cap{expected + L"\n"};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(expected, result);
    }

    TEST_METHOD(Terminal_Confirm)
    {
        // The prompt is written inline on stdout with the standard suffix appended, and nothing
        // goes to stderr.
        {
            InputCaptureTerminal cap{L"y\n"};
            VERIFY_IS_TRUE(cap.terminal.Confirm(L"Remove everything?"));
            VERIFY_ARE_EQUAL(std::wstring{L"Remove everything? [y/N] "}, cap.outPipe.captured());
            VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
        }

        // Only a bare y accepts, in either case and with surrounding whitespace trimmed.
        for (const auto* answer : {L"y\n", L"Y\n", L"  y  \n"})
        {
            InputCaptureTerminal cap{answer};
            VERIFY_IS_TRUE(cap.terminal.Confirm(L"Remove everything?"));
        }

        // Anything else declines, including a spelled-out yes, matching the container CLI ecosystem.
        // A prune with no input attached must abort rather than block, so end of input declines too.
        for (const auto* answer : {L"yes\n", L"n\n", L"N\n", L"no\n", L"\n", L"maybe\n", L""})
        {
            InputCaptureTerminal cap{answer};
            VERIFY_IS_FALSE(cap.terminal.Confirm(L"Remove everything?"));
            VERIFY_ARE_EQUAL(std::wstring{L"Remove everything? [y/N] "}, cap.outPipe.captured());
        }

        // The message is a formatting argument, not a format string, so braces must not be
        // interpreted.
        {
            InputCaptureTerminal cap{L"y\n"};
            const std::wstring message = L"Remove {} {0} {name} 100%?";

            VERIFY_IS_TRUE(cap.terminal.Confirm(message));
            VERIFY_ARE_EQUAL(message + L" [y/N] ", cap.outPipe.captured());
        }
    }

    TEST_METHOD(Terminal_ReadLineMaskDefaultsToUnmasked)
    {
        // ReadLine(bool mask = false): the default reads without masking and returns
        // the line, used by the --password-stdin path.
        InputCaptureTerminal cap{L"piped-secret\n"};
        VERIFY_ARE_EQUAL(std::wstring{L"piped-secret"}, cap.terminal.ReadLine().value_or(L"<eof>"));
        // Nothing is written for a bare ReadLine (no prompt label).
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
    }
};

} // namespace WSLCCLITerminalUnitTests
