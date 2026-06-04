/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIVTSupportUnitTests.cpp

Abstract:

    This file contains unit tests for VT sequence construction and console mode helpers.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "VTSupport.h"

using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;
using namespace wsl::windows::common::vt;

namespace WSLCCLIVTSupportUnitTests {

// Creates a real console screen buffer that can be used as an output handle for console API tests.
// The buffer is not attached to the visible console window, so it does not affect the test runner output.
static wil::unique_hfile MakeScreenBuffer()
{
    wil::unique_hfile handle{CreateConsoleScreenBuffer(
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr)};
    THROW_LAST_ERROR_IF(!handle);
    return handle;
}

class WSLCCLIVTSupportUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIVTSupportUnitTests)

    TEST_METHOD(VT_Sequence)
    {
        // Constant sequence round-trips correctly.
        const Sequence constant{"\x1b[0m"};
        VERIFY_ARE_EQUAL("\x1b[0m", constant);

        // Default-constructed ConstructedSequence is empty.
        ConstructedSequence empty;
        VERIFY_IS_TRUE(empty.Get().empty());

        // Construct from string.
        ConstructedSequence seq{"\x1b[1m"};
        VERIFY_ARE_EQUAL("\x1b[1m", seq);

        // Append combines sequences.
        seq.Append(Sequence{"\x1b[91m"});
        VERIFY_ARE_EQUAL("\x1b[1m\x1b[91m", seq);

        // Appending an empty sequence is a no-op.
        seq.Append(Sequence{});
        VERIFY_ARE_EQUAL("\x1b[1m\x1b[91m", seq);

        // Clear resets to empty.
        seq.Clear();
        VERIFY_IS_TRUE(seq.Get().empty());

        // Copy construction.
        ConstructedSequence original{"\x1b[1m"};
        ConstructedSequence copy{original};
        VERIFY_ARE_EQUAL(original.Get(), copy.Get());

        // Move construction.
        ConstructedSequence moved{std::move(original)};
        VERIFY_ARE_EQUAL("\x1b[1m", moved);

        // Copy assignment.
        ConstructedSequence assigned;
        assigned = copy;
        VERIFY_ARE_EQUAL(copy.Get(), assigned.Get());

        // Move assignment.
        ConstructedSequence moveAssigned;
        moveAssigned = std::move(moved);
        VERIFY_ARE_EQUAL("\x1b[1m", moveAssigned);

        // SGR Construction.
        VERIFY_ARE_EQUAL("\x1b[1;31m", Sgr({1, 31}));
        VERIFY_ARE_EQUAL("\x1b[0m", Sgr({0}));
    }

    TEST_METHOD(VT_CursorSequences)
    {
        VERIFY_ARE_EQUAL("\x1b[3A", Cursor::Up(3));
        VERIFY_ARE_EQUAL("\x1b[5B", Cursor::Down(5));
        VERIFY_ARE_EQUAL("\x1b[2C", Cursor::Forward(2));
        VERIFY_ARE_EQUAL("\x1b[1D", Cursor::Backward(1));
        VERIFY_ARE_EQUAL("\x1b[5;10H", Cursor::MoveTo(5, 10));
        VERIFY_ARE_EQUAL("\x1b[H", Cursor::Home);

        VERIFY_THROWS_SPECIFIC(
            Cursor::Up(-1), wil::ResultException, [](const wil::ResultException& e) { return e.GetErrorCode() == E_INVALIDARG; });
        VERIFY_THROWS_SPECIFIC(Cursor::MoveTo(0, 1), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_INVALIDARG;
        });
        VERIFY_THROWS_SPECIFIC(Cursor::MoveTo(1, 0), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_INVALIDARG;
        });

        VERIFY_ARE_EQUAL("\x1b[?2004h", Cursor::BracketedPasteOn);
        VERIFY_ARE_EQUAL("\x1b[?2004l", Cursor::BracketedPasteOff);
    }

    TEST_METHOD(VT_TextFormatSequences)
    {
        VERIFY_ARE_EQUAL("\x1b[0m", Format::Default);
        VERIFY_ARE_EQUAL("\x1b[7m", Format::Negative);
        VERIFY_ARE_EQUAL("\x1b[1m", Format::Bright);
        VERIFY_ARE_EQUAL("\x1b[2m", Format::Dim);
        VERIFY_ARE_EQUAL("\x1b[22m", Format::Normal);
        VERIFY_ARE_EQUAL("\x1b[3m", Format::Italic);
        VERIFY_ARE_EQUAL("\x1b[23m", Format::NoItalic);
        VERIFY_ARE_EQUAL("\x1b[4m", Format::Underline);
        VERIFY_ARE_EQUAL("\x1b[24m", Format::NoUnderline);

        VERIFY_ARE_EQUAL("\x1b[30m", Format::Fg::Black);
        VERIFY_ARE_EQUAL("\x1b[31m", Format::Fg::Red);
        VERIFY_ARE_EQUAL("\x1b[32m", Format::Fg::Green);
        VERIFY_ARE_EQUAL("\x1b[33m", Format::Fg::Yellow);
        VERIFY_ARE_EQUAL("\x1b[34m", Format::Fg::Blue);
        VERIFY_ARE_EQUAL("\x1b[35m", Format::Fg::Magenta);
        VERIFY_ARE_EQUAL("\x1b[36m", Format::Fg::Cyan);
        VERIFY_ARE_EQUAL("\x1b[37m", Format::Fg::White);

        VERIFY_ARE_EQUAL("\x1b[90m", Format::Fg::BrightBlack);
        VERIFY_ARE_EQUAL("\x1b[91m", Format::Fg::BrightRed);
        VERIFY_ARE_EQUAL("\x1b[92m", Format::Fg::BrightGreen);
        VERIFY_ARE_EQUAL("\x1b[93m", Format::Fg::BrightYellow);
        VERIFY_ARE_EQUAL("\x1b[94m", Format::Fg::BrightBlue);
        VERIFY_ARE_EQUAL("\x1b[95m", Format::Fg::BrightMagenta);
        VERIFY_ARE_EQUAL("\x1b[96m", Format::Fg::BrightCyan);
        VERIFY_ARE_EQUAL("\x1b[97m", Format::Fg::BrightWhite);

        VERIFY_ARE_EQUAL("\x1b[38;2;255;128;0m", Format::Fg::Extended(Format::Color{255, 128, 0}));

        VERIFY_ARE_EQUAL("\x1b[40m", Format::Bg::Black);
        VERIFY_ARE_EQUAL("\x1b[41m", Format::Bg::Red);
        VERIFY_ARE_EQUAL("\x1b[42m", Format::Bg::Green);
        VERIFY_ARE_EQUAL("\x1b[43m", Format::Bg::Yellow);
        VERIFY_ARE_EQUAL("\x1b[44m", Format::Bg::Blue);
        VERIFY_ARE_EQUAL("\x1b[45m", Format::Bg::Magenta);
        VERIFY_ARE_EQUAL("\x1b[46m", Format::Bg::Cyan);
        VERIFY_ARE_EQUAL("\x1b[47m", Format::Bg::White);

        VERIFY_ARE_EQUAL("\x1b[100m", Format::Bg::BrightBlack);
        VERIFY_ARE_EQUAL("\x1b[101m", Format::Bg::BrightRed);
        VERIFY_ARE_EQUAL("\x1b[102m", Format::Bg::BrightGreen);
        VERIFY_ARE_EQUAL("\x1b[103m", Format::Bg::BrightYellow);
        VERIFY_ARE_EQUAL("\x1b[104m", Format::Bg::BrightBlue);
        VERIFY_ARE_EQUAL("\x1b[105m", Format::Bg::BrightMagenta);
        VERIFY_ARE_EQUAL("\x1b[106m", Format::Bg::BrightCyan);
        VERIFY_ARE_EQUAL("\x1b[107m", Format::Bg::BrightWhite);

        VERIFY_ARE_EQUAL("\x1b[48;2;0;64;192m", Format::Bg::Extended(Format::Color{0, 64, 192}));

        VERIFY_ARE_EQUAL(
            "\x1b]8;;https://example.com\x1b\\Click here\x1b]8;;\x1b\\", Format::Hyperlink("Click here", "https://example.com"));
    }

    TEST_METHOD(VT_EraseSequences)
    {
        VERIFY_ARE_EQUAL("\x1b[0K", Erase::LineForward);
        VERIFY_ARE_EQUAL("\x1b[1K", Erase::LineBackward);
        VERIFY_ARE_EQUAL("\x1b[2K", Erase::LineEntirely);
        VERIFY_ARE_EQUAL("\x1b[J", Erase::ScreenForward);
        VERIFY_ARE_EQUAL("\x1b[1J", Erase::ScreenBackward);
        VERIFY_ARE_EQUAL("\x1b[2J", Erase::ScreenEntirely);
    }

    TEST_METHOD(VT_ProgressSequences)
    {
        VERIFY_ARE_EQUAL("\x1b]9;4;0;0\x1b\\", Progress::Construct(Progress::State::None));
        VERIFY_ARE_EQUAL("\x1b]9;4;3;0\x1b\\", Progress::Construct(Progress::State::Indeterminate));
        VERIFY_ARE_EQUAL("\x1b]9;4;1;50\x1b\\", Progress::Construct(Progress::State::Normal, 50u));
        VERIFY_ARE_EQUAL("\x1b]9;4;2;75\x1b\\", Progress::Construct(Progress::State::Error, 75u));
        VERIFY_ARE_EQUAL("\x1b]9;4;4;25\x1b\\", Progress::Construct(Progress::State::Paused, 25u));

        VERIFY_THROWS_SPECIFIC(Progress::Construct(Progress::State::Normal), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_INVALIDARG;
        });
        VERIFY_THROWS_SPECIFIC(Progress::Construct(Progress::State::Normal, 101u), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_BOUNDS;
        });
    }

    TEST_METHOD(VT_StringConcatenation)
    {
        // Sequence + Sequence
        VERIFY_ARE_EQUAL("\x1b[1m\x1b[0m", Format::Bright + Format::Default);

        // Sequence + string literal
        VERIFY_ARE_EQUAL("\x1b[1mhello", Format::Bright + "hello");

        // string literal + Sequence
        VERIFY_ARE_EQUAL("hello\x1b[0m", "hello" + Format::Default);

        // Sequence + std::string
        VERIFY_ARE_EQUAL("\x1b[1mhello", Format::Bright + std::string{"hello"});

        // std::string + Sequence
        VERIFY_ARE_EQUAL("world\x1b[0m", std::string{"world"} + Format::Default);

        // Chained: Sequence + Sequence + literal — verifies operator+ associativity.
        VERIFY_ARE_EQUAL("\x1b[?2004h\x1b[91mroot@ ", Cursor::BracketedPasteOn + Format::Fg::BrightRed + "root@ ");

        // Sequence + std::wstring
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[0mworld"}, Format::Default + std::wstring{L"world"});

        // std::wstring + Sequence
        VERIFY_ARE_EQUAL(std::wstring{L"world\x1b[0m"}, std::wstring{L"world"} + Format::Default);
    }

    TEST_METHOD(VT_StreamOperators)
    {
        const Sequence seq{"\x1b[1m"};

        // Narrow stream writes bytes as-is.
        std::ostringstream oss;
        oss << seq;
        VERIFY_ARE_EQUAL(std::string{"\x1b[1m"}, oss.str());

        // Multiple sequences can be streamed in one expression.
        std::ostringstream multi;
        multi << Format::Bright << "text" << Format::Default;
        VERIFY_ARE_EQUAL(std::string{"\x1b[1mtext\x1b[0m"}, multi.str());

        // Wide stream correctly widens the ASCII sequence bytes.
        std::wostringstream woss;
        woss << seq;
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[1m"}, woss.str());

        // std::format — narrow.
        VERIFY_ARE_EQUAL(std::string{"\x1b[92mhello\x1b[0m"}, std::format("{}{}{}", Format::Fg::BrightGreen, "hello", Format::Default));

        // std::format — wide, sequence bytes widened losslessly.
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[92mhello\x1b[0m"}, std::format(L"{}{}{}", Format::Fg::BrightGreen, L"hello", Format::Default));

        // std::format — ConstructedSequence via Sequence base.
        VERIFY_ARE_EQUAL(std::string{"\x1b[3A text"}, std::format("{} text", Cursor::Up(3)));

        // std::format — Sgr multi-parameter sequence.
        VERIFY_ARE_EQUAL(std::string{"\x1b[1;31mtext\x1b[0m"}, std::format("{}text{}", Sgr({1, 31}), Format::Default));
    }

    TEST_METHOD(VT_ChangeTerminalMode)
    {
        auto buffer = MakeScreenBuffer();
        HANDLE h = buffer.get();

        // Capture the original cursor visibility.
        CONSOLE_CURSOR_INFO original{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleCursorInfo(h, &original));

        {
            // Hide the cursor.
            ChangeTerminalMode hide{h, false};
            VERIFY_IS_TRUE(hide.IsConsole());

            CONSOLE_CURSOR_INFO info{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleCursorInfo(h, &info));
            VERIFY_IS_FALSE(!!info.bVisible);
        }

        // Destructor must restore the original visibility.
        CONSOLE_CURSOR_INFO restored{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleCursorInfo(h, &restored));
        VERIFY_ARE_EQUAL(original.bVisible, restored.bVisible);

        {
            // Show the cursor explicitly.
            ChangeTerminalMode show{h, true};
            CONSOLE_CURSOR_INFO info{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleCursorInfo(h, &info));
            VERIFY_IS_TRUE(!!info.bVisible);
        }

        // Non-console handle (a pipe) is silently ignored — IsConsole() returns false.
        wil::unique_handle readPipe, writePipe;
        VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&readPipe, &writePipe, nullptr, 0));
        ChangeTerminalMode nonConsole{readPipe.get(), false};
        VERIFY_IS_FALSE(nonConsole.IsConsole());
    }

    TEST_METHOD(VT_ChangeTerminalMode_RedirectedHandles)
    {
        wil::unique_hfile file{
            CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        VERIFY_IS_TRUE(!!file);
        {
            ChangeTerminalMode mode{file.get(), false};
            VERIFY_IS_FALSE(mode.IsConsole());
        }

        // NULL handle.
        {
            ChangeTerminalMode mode{nullptr, false};
            VERIFY_IS_FALSE(mode.IsConsole());
        }

        // INVALID_HANDLE_VALUE.
        {
            ChangeTerminalMode mode{INVALID_HANDLE_VALUE, false};
            VERIFY_IS_FALSE(mode.IsConsole());
        }
    }

    TEST_METHOD(VT_EnableVirtualTerminal)
    {
        auto buffer = MakeScreenBuffer();
        HANDLE h = buffer.get();

        DWORD baseline{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &baseline));
        SetConsoleMode(h, baseline & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        {
            EnableVirtualTerminal vt{h};
            VERIFY_IS_TRUE(vt.IsVTEnabled());

            DWORD mode{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &mode));
            VERIFY_IS_TRUE(!!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING));
        }

        DWORD restored{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &restored));
        VERIFY_IS_FALSE(!!(restored & ENABLE_VIRTUAL_TERMINAL_PROCESSING));

        // With DISABLE_NEWLINE_AUTO_RETURN requested.
        {
            EnableVirtualTerminal vtWithNewline{h, EnableVirtualTerminal::Mode::Output, true};
            VERIFY_IS_TRUE(vtWithNewline.IsVTEnabled());

            DWORD mode{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &mode));
            VERIFY_IS_TRUE(!!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING));
        }

        // Input mode.
        wil::unique_hfile conin{CreateFileW(
            L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        VERIFY_IS_TRUE(!!conin);

        DWORD inputBaseline{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &inputBaseline));
        {
            EnableVirtualTerminal vt{conin.get(), EnableVirtualTerminal::Mode::Input};
            VERIFY_IS_TRUE(vt.IsVTEnabled());

            DWORD mode{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &mode));
            VERIFY_IS_TRUE(!!(mode & ENABLE_VIRTUAL_TERMINAL_INPUT));
            VERIFY_IS_FALSE(!!(mode & ENABLE_LINE_INPUT));
            VERIFY_IS_FALSE(!!(mode & ENABLE_ECHO_INPUT));
        }

        DWORD inputRestored{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &inputRestored));
        VERIFY_ARE_EQUAL(inputBaseline, inputRestored);

        // Non-console handles are silently ignored for both modes.
        wil::unique_handle readPipe, writePipe;
        VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&readPipe, &writePipe, nullptr, 0));
        VERIFY_IS_FALSE(EnableVirtualTerminal(readPipe.get(), EnableVirtualTerminal::Mode::Output).IsVTEnabled());
        VERIFY_IS_FALSE(EnableVirtualTerminal(readPipe.get(), EnableVirtualTerminal::Mode::Input).IsVTEnabled());
    }

    TEST_METHOD(VT_EnableVirtualTerminal_RedirectedHandles)
    {
        wil::unique_hfile file{
            CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        VERIFY_IS_TRUE(!!file);

        VERIFY_IS_FALSE(EnableVirtualTerminal(file.get(), EnableVirtualTerminal::Mode::Output).IsVTEnabled());
        VERIFY_IS_FALSE(EnableVirtualTerminal(file.get(), EnableVirtualTerminal::Mode::Input).IsVTEnabled());
        VERIFY_IS_FALSE(EnableVirtualTerminal(nullptr, EnableVirtualTerminal::Mode::Output).IsVTEnabled());
        VERIFY_IS_FALSE(EnableVirtualTerminal(nullptr, EnableVirtualTerminal::Mode::Input).IsVTEnabled());
        VERIFY_IS_FALSE(EnableVirtualTerminal(INVALID_HANDLE_VALUE, EnableVirtualTerminal::Mode::Output).IsVTEnabled());
        VERIFY_IS_FALSE(EnableVirtualTerminal(INVALID_HANDLE_VALUE, EnableVirtualTerminal::Mode::Input).IsVTEnabled());
    }

    TEST_METHOD(VT_PrimaryDeviceAttributes)
    {
        // Feed a synthetic DA1 response: ESC [ ? 62 ; 1 ; 4 c
        // Conformance level 62 (VT400), extensions: Columns132 (1), Sixel (4).
        // ExtractSequence is exercised indirectly through PrimaryDeviceAttributes.
        std::ostringstream out;
        std::istringstream in{"\x1b[?62;1;4c"};

        PrimaryDeviceAttributes da{out, in};

        VERIFY_IS_TRUE(da.Supports(PrimaryDeviceAttributes::Extension::Columns132));
        VERIFY_IS_TRUE(da.Supports(PrimaryDeviceAttributes::Extension::Sixel));
        VERIFY_IS_FALSE(da.Supports(PrimaryDeviceAttributes::Extension::PrinterPort));

        // Verify the DA1 request was sent.
        VERIFY_ARE_EQUAL("\x1b[0c", out.str());
    }

    TEST_METHOD(VT_PrimaryDeviceAttributes_Empty)
    {
        // Empty/malformed response — should not throw, extensions remain unset.
        std::ostringstream out;
        std::istringstream in{""};

        PrimaryDeviceAttributes da{out, in};

        VERIFY_IS_FALSE(da.Supports(PrimaryDeviceAttributes::Extension::Columns132));
    }
};

} // namespace WSLCCLIVTSupportUnitTests
