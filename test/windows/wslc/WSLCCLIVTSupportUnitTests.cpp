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
        const Sequence constant{L"\x1b[0m"};
        VERIFY_ARE_EQUAL(L"\x1b[0m", constant);

        // Default-constructed ConstructedSequence is empty.
        ConstructedSequence empty;
        VERIFY_IS_TRUE(empty.Get().empty());

        // Construct from string.
        ConstructedSequence seq{L"\x1b[1m"};
        VERIFY_ARE_EQUAL(L"\x1b[1m", seq);

        // Append combines sequences.
        seq.Append(Sequence{L"\x1b[91m"});
        VERIFY_ARE_EQUAL(L"\x1b[1m\x1b[91m", seq);

        // Appending an empty sequence is a no-op.
        seq.Append(Sequence{});
        VERIFY_ARE_EQUAL(L"\x1b[1m\x1b[91m", seq);

        // Clear resets to empty.
        seq.Clear();
        VERIFY_IS_TRUE(seq.Get().empty());

        // Copy construction.
        ConstructedSequence original{L"\x1b[1m"};
        ConstructedSequence copy{original};
        VERIFY_ARE_EQUAL(original.Get(), copy.Get());

        // Move construction.
        ConstructedSequence moved{std::move(original)};
        VERIFY_ARE_EQUAL(L"\x1b[1m", moved);

        // Copy assignment.
        ConstructedSequence assigned;
        assigned = copy;
        VERIFY_ARE_EQUAL(copy.Get(), assigned.Get());

        // Move assignment.
        ConstructedSequence moveAssigned;
        moveAssigned = std::move(moved);
        VERIFY_ARE_EQUAL(L"\x1b[1m", moveAssigned);

        // SGR Construction.
        VERIFY_ARE_EQUAL(L"\x1b[1;31m", Sgr({1, 31}));
        VERIFY_ARE_EQUAL(L"\x1b[0m", Sgr({0}));
    }

    TEST_METHOD(VT_CursorSequences)
    {
        VERIFY_ARE_EQUAL(L"\x1b[3A", Cursor::Up(3));
        VERIFY_ARE_EQUAL(L"\x1b[5B", Cursor::Down(5));
        VERIFY_ARE_EQUAL(L"\x1b[2C", Cursor::Forward(2));
        VERIFY_ARE_EQUAL(L"\x1b[1D", Cursor::Backward(1));
        VERIFY_ARE_EQUAL(L"\x1b[5;10H", Cursor::MoveTo(5, 10));
        VERIFY_ARE_EQUAL(L"\x1b[H", Cursor::Home);

        // cells == 0 is a no-op — returns an empty sequence rather than
        // emitting ESC[0X, which most terminals treat as "move 1 cell".
        VERIFY_IS_TRUE(Cursor::Up(0).Get().empty());
        VERIFY_IS_TRUE(Cursor::Down(0).Get().empty());
        VERIFY_IS_TRUE(Cursor::Forward(0).Get().empty());
        VERIFY_IS_TRUE(Cursor::Backward(0).Get().empty());

        VERIFY_THROWS_SPECIFIC(
            Cursor::Up(-1), wil::ResultException, [](const wil::ResultException& e) { return e.GetErrorCode() == E_INVALIDARG; });
        VERIFY_THROWS_SPECIFIC(Cursor::MoveTo(0, 1), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_INVALIDARG;
        });
        VERIFY_THROWS_SPECIFIC(Cursor::MoveTo(1, 0), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_INVALIDARG;
        });

        VERIFY_ARE_EQUAL(L"\x1b[?2004h", Cursor::BracketedPasteOn);
        VERIFY_ARE_EQUAL(L"\x1b[?2004l", Cursor::BracketedPasteOff);
    }

    TEST_METHOD(VT_TextFormatSequences)
    {
        VERIFY_ARE_EQUAL(L"\x1b[0m", Format::Default);
        VERIFY_ARE_EQUAL(L"\x1b[7m", Format::Negative);
        VERIFY_ARE_EQUAL(L"\x1b[1m", Format::Bright);
        VERIFY_ARE_EQUAL(L"\x1b[2m", Format::Dim);
        VERIFY_ARE_EQUAL(L"\x1b[22m", Format::Normal);
        VERIFY_ARE_EQUAL(L"\x1b[3m", Format::Italic);
        VERIFY_ARE_EQUAL(L"\x1b[23m", Format::NoItalic);
        VERIFY_ARE_EQUAL(L"\x1b[4m", Format::Underline);
        VERIFY_ARE_EQUAL(L"\x1b[24m", Format::NoUnderline);

        VERIFY_ARE_EQUAL(L"\x1b[30m", Format::Fg::Black);
        VERIFY_ARE_EQUAL(L"\x1b[31m", Format::Fg::Red);
        VERIFY_ARE_EQUAL(L"\x1b[32m", Format::Fg::Green);
        VERIFY_ARE_EQUAL(L"\x1b[33m", Format::Fg::Yellow);
        VERIFY_ARE_EQUAL(L"\x1b[34m", Format::Fg::Blue);
        VERIFY_ARE_EQUAL(L"\x1b[35m", Format::Fg::Magenta);
        VERIFY_ARE_EQUAL(L"\x1b[36m", Format::Fg::Cyan);
        VERIFY_ARE_EQUAL(L"\x1b[37m", Format::Fg::White);

        VERIFY_ARE_EQUAL(L"\x1b[90m", Format::Fg::BrightBlack);
        VERIFY_ARE_EQUAL(L"\x1b[91m", Format::Fg::BrightRed);
        VERIFY_ARE_EQUAL(L"\x1b[92m", Format::Fg::BrightGreen);
        VERIFY_ARE_EQUAL(L"\x1b[93m", Format::Fg::BrightYellow);
        VERIFY_ARE_EQUAL(L"\x1b[94m", Format::Fg::BrightBlue);
        VERIFY_ARE_EQUAL(L"\x1b[95m", Format::Fg::BrightMagenta);
        VERIFY_ARE_EQUAL(L"\x1b[96m", Format::Fg::BrightCyan);
        VERIFY_ARE_EQUAL(L"\x1b[97m", Format::Fg::BrightWhite);

        VERIFY_ARE_EQUAL(L"\x1b[38;2;255;128;0m", Format::Fg::Extended(Format::Color{255, 128, 0}));

        VERIFY_ARE_EQUAL(L"\x1b[40m", Format::Bg::Black);
        VERIFY_ARE_EQUAL(L"\x1b[41m", Format::Bg::Red);
        VERIFY_ARE_EQUAL(L"\x1b[42m", Format::Bg::Green);
        VERIFY_ARE_EQUAL(L"\x1b[43m", Format::Bg::Yellow);
        VERIFY_ARE_EQUAL(L"\x1b[44m", Format::Bg::Blue);
        VERIFY_ARE_EQUAL(L"\x1b[45m", Format::Bg::Magenta);
        VERIFY_ARE_EQUAL(L"\x1b[46m", Format::Bg::Cyan);
        VERIFY_ARE_EQUAL(L"\x1b[47m", Format::Bg::White);

        VERIFY_ARE_EQUAL(L"\x1b[100m", Format::Bg::BrightBlack);
        VERIFY_ARE_EQUAL(L"\x1b[101m", Format::Bg::BrightRed);
        VERIFY_ARE_EQUAL(L"\x1b[102m", Format::Bg::BrightGreen);
        VERIFY_ARE_EQUAL(L"\x1b[103m", Format::Bg::BrightYellow);
        VERIFY_ARE_EQUAL(L"\x1b[104m", Format::Bg::BrightBlue);
        VERIFY_ARE_EQUAL(L"\x1b[105m", Format::Bg::BrightMagenta);
        VERIFY_ARE_EQUAL(L"\x1b[106m", Format::Bg::BrightCyan);
        VERIFY_ARE_EQUAL(L"\x1b[107m", Format::Bg::BrightWhite);

        VERIFY_ARE_EQUAL(L"\x1b[48;2;0;64;192m", Format::Bg::Extended(Format::Color{0, 64, 192}));

        VERIFY_ARE_EQUAL(
            L"\x1b]8;;https://example.com\x1b\\Click here\x1b]8;;\x1b\\",
            Format::Hyperlink(L"Click here", L"https://example.com"));
    }

    TEST_METHOD(VT_EraseSequences)
    {
        VERIFY_ARE_EQUAL(L"\x1b[K", Erase::LineForward);
        VERIFY_ARE_EQUAL(L"\x1b[1K", Erase::LineBackward);
        VERIFY_ARE_EQUAL(L"\x1b[2K", Erase::LineEntirely);
        VERIFY_ARE_EQUAL(L"\x1b[J", Erase::ScreenForward);
        VERIFY_ARE_EQUAL(L"\x1b[1J", Erase::ScreenBackward);
        VERIFY_ARE_EQUAL(L"\x1b[2J", Erase::ScreenEntirely);
    }

    TEST_METHOD(VT_ProgressSequences)
    {
        VERIFY_ARE_EQUAL(L"\x1b]9;4;0;0\x1b\\", Progress::Construct(Progress::State::None));
        VERIFY_ARE_EQUAL(L"\x1b]9;4;3;0\x1b\\", Progress::Construct(Progress::State::Indeterminate));
        VERIFY_ARE_EQUAL(L"\x1b]9;4;1;50\x1b\\", Progress::Construct(Progress::State::Normal, 50u));
        VERIFY_ARE_EQUAL(L"\x1b]9;4;2;75\x1b\\", Progress::Construct(Progress::State::Error, 75u));
        VERIFY_ARE_EQUAL(L"\x1b]9;4;4;25\x1b\\", Progress::Construct(Progress::State::Paused, 25u));

        VERIFY_THROWS_SPECIFIC(Progress::Construct(Progress::State::Normal), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_INVALIDARG;
        });
        VERIFY_THROWS_SPECIFIC(Progress::Construct(Progress::State::Normal, 101u), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_BOUNDS;
        });
    }

    TEST_METHOD(VT_IsColor)
    {
        // Named SGR sequences are color.
        VERIFY_IS_TRUE(Format::Bright.IsColor());
        VERIFY_IS_TRUE(Format::Dim.IsColor());
        VERIFY_IS_TRUE(Format::Fg::BrightRed.IsColor());
        VERIFY_IS_TRUE(Format::Default.IsColor());

        // Constructed multi-param SGR is color.
        VERIFY_IS_TRUE(Sgr({1, 31}).IsColor());

        // OSC 8 hyperlink is color-adjacent.
        VERIFY_IS_TRUE(Format::Hyperlink(L"text", L"https://example.com").IsColor());

        // Cursor movement is structural — not color.
        VERIFY_IS_FALSE(Cursor::Up(1).IsColor());
        VERIFY_IS_FALSE(Cursor::Home.IsColor());

        // Erase is structural — not color.
        VERIFY_IS_FALSE(Erase::LineForward.IsColor());
        VERIFY_IS_FALSE(Erase::ScreenForward.IsColor());

        // Progress is structural — not color.
        VERIFY_IS_FALSE(Progress::Construct(Progress::State::Normal, 50u).IsColor());
    }

    TEST_METHOD(VT_StringConcatenation)
    {
        // Sequence + Sequence
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[1m\x1b[0m"}, Format::Bright + Format::Default);

        // Sequence + string literal
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[1mhello"}, Format::Bright + L"hello");

        // string literal + Sequence
        VERIFY_ARE_EQUAL(std::wstring{L"hello\x1b[0m"}, L"hello" + Format::Default);

        // Sequence + std::wstring
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[1mhello"}, Format::Bright + std::wstring{L"hello"});

        // std::wstring + Sequence
        VERIFY_ARE_EQUAL(std::wstring{L"world\x1b[0m"}, std::wstring{L"world"} + Format::Default);

        // Chained: Sequence + Sequence + literal — verifies operator+ associativity.
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[?2004h\x1b[91mroot@ "}, Cursor::BracketedPasteOn + Format::Fg::BrightRed + L"root@ ");

        // In-place wide append.
        std::wstring buf;
        buf += Format::Default;
        buf += std::wstring{L"world"};
        VERIFY_ARE_EQUAL(std::wstring{L"\x1b[0mworld"}, buf);
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
        VERIFY_WIN32_BOOL_SUCCEEDED(SetConsoleMode(h, baseline & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING));

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

        // Non-console handles are silently ignored for both modes.
        wil::unique_handle readPipe, writePipe;
        VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&readPipe, &writePipe, nullptr, 0));
        VERIFY_IS_FALSE(EnableVirtualTerminal(readPipe.get(), EnableVirtualTerminal::Mode::Output).IsVTEnabled());
        VERIFY_IS_FALSE(EnableVirtualTerminal(readPipe.get(), EnableVirtualTerminal::Mode::Input).IsVTEnabled());
    }

    TEST_METHOD(VT_EnableVirtualTerminal_InputMode)
    {
        // CONIN$ requires an attached console. CI environments that run without one
        // (e.g. headless agents) cannot exercise this path; skip rather than fail.
        wil::unique_hfile conin{CreateFileW(
            L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        if (!conin)
        {
            LogSkipped("Skipping input-mode VT test: CONIN$ is not available (no attached console)");
            return;
        }

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

    TEST_METHOD(VT_EnableVirtualTerminal_AlreadyEnabled_Output)
    {
        // Regression: when VT_PROC is already set on the console, EnableVirtualTerminal
        // must report VT as enabled (so callers gate VT output correctly) AND must not
        // claim restore ownership — destruction must leave the pre-existing mode alone.
        auto buffer = MakeScreenBuffer();
        HANDLE h = buffer.get();

        DWORD baseline{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &baseline));
        const DWORD scopeExitRestore = baseline;
        auto restoreBaseline = wil::scope_exit([&] { ::SetConsoleMode(h, scopeExitRestore); });

        // Pre-enable VT processing so the constructor's tryEnable() short-circuits
        // on the "flags already set" path.
        const DWORD preEnabled = baseline | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        VERIFY_WIN32_BOOL_SUCCEEDED(SetConsoleMode(h, preEnabled));

        {
            EnableVirtualTerminal vt{h};
            VERIFY_IS_TRUE(vt.IsVTEnabled(), L"VT must be reported as enabled when it was already enabled on the handle");

            DWORD mode{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &mode));
            VERIFY_ARE_EQUAL(preEnabled, mode, L"Constructor must not change the mode when the requested flags are already set");
        }

        // Destructor must not have restored anything — the pre-existing VT_PROC
        // bit must still be set after the EnableVirtualTerminal instance goes
        // out of scope.
        DWORD afterScope{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &afterScope));
        VERIFY_ARE_EQUAL(preEnabled, afterScope, L"Destructor must not restore mode when constructor did not change it");
    }

    TEST_METHOD(VT_EnableVirtualTerminal_AlreadyEnabled_OutputWithDisableNewlineAutoReturn)
    {
        // Same regression as above, exercising the disableNewlineAutoReturn=true path
        // where the constructor tries (VT_PROC | DISABLE_NEWLINE_AUTO_RETURN) first.
        auto buffer = MakeScreenBuffer();
        HANDLE h = buffer.get();

        DWORD baseline{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &baseline));
        const DWORD scopeExitRestore = baseline;
        auto restoreBaseline = wil::scope_exit([&] { ::SetConsoleMode(h, scopeExitRestore); });

        const DWORD preEnabled = baseline | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
        if (!SetConsoleMode(h, preEnabled))
        {
            // DISABLE_NEWLINE_AUTO_RETURN is not supported on all conhost builds; skip
            // rather than fail in environments where it cannot be set.
            LogSkipped("Skipping DISABLE_NEWLINE_AUTO_RETURN test: SetConsoleMode rejected the flag");
            return;
        }

        {
            EnableVirtualTerminal vt{h, EnableVirtualTerminal::Mode::Output, true};
            VERIFY_IS_TRUE(vt.IsVTEnabled());

            DWORD mode{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &mode));
            VERIFY_ARE_EQUAL(preEnabled, mode);
        }

        DWORD afterScope{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(h, &afterScope));
        VERIFY_ARE_EQUAL(preEnabled, afterScope);
    }

    TEST_METHOD(VT_EnableVirtualTerminal_AlreadyEnabled_Input)
    {
        // Regression: when CONIN$ is already in the exact target mode
        // (ENABLE_VIRTUAL_TERMINAL_INPUT + ENABLE_EXTENDED_FLAGS, no ENABLE_LINE_INPUT,
        // no ENABLE_ECHO_INPUT), the constructor's "no change needed" early-return
        // must still report IsVTEnabled()==true and must not touch the mode.
        wil::unique_hfile conin{CreateFileW(
            L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        if (!conin)
        {
            LogSkipped("Skipping input-mode VT already-enabled test: CONIN$ is not available (no attached console)");
            return;
        }

        DWORD baseline{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &baseline));
        const DWORD scopeExitRestore = baseline;
        auto restoreBaseline = wil::scope_exit([&] { ::SetConsoleMode(conin.get(), scopeExitRestore); });

        // Pre-configure CONIN$ to exactly match what EnableVirtualTerminal would set,
        // so the constructor takes the newMode == current early-return path.
        const DWORD preEnabled = (baseline & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT;
        VERIFY_WIN32_BOOL_SUCCEEDED(SetConsoleMode(conin.get(), preEnabled));

        {
            EnableVirtualTerminal vt{conin.get(), EnableVirtualTerminal::Mode::Input};
            VERIFY_IS_TRUE(vt.IsVTEnabled(), L"VT input must be reported as enabled when CONIN$ was already in the target mode");

            DWORD mode{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &mode));
            VERIFY_ARE_EQUAL(preEnabled, mode, L"Constructor must not change input mode when it already matches the target");
        }

        DWORD afterScope{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &afterScope));
        VERIFY_ARE_EQUAL(preEnabled, afterScope, L"Destructor must not restore input mode when constructor did not change it");
    }

    TEST_METHOD(VT_PrimaryDeviceAttributes)
    {
        // Clean DA1 response: conformance level 62, extensions Columns132 (1) and Sixel (4).
        {
            std::wostringstream out;
            std::wistringstream in{L"\x1b[?62;1;4c"};
            PrimaryDeviceAttributes da{out, in};

            VERIFY_IS_TRUE(da.Supports(PrimaryDeviceAttributes::Extension::Columns132));
            VERIFY_IS_TRUE(da.Supports(PrimaryDeviceAttributes::Extension::Sixel));
            VERIFY_IS_FALSE(da.Supports(PrimaryDeviceAttributes::Extension::PrinterPort));
            // DA1 request must have been written to the output stream.
            VERIFY_ARE_EQUAL(std::wstring{L"\x1b[0c"}, out.str());
        }

        // Trailing plain text (e.g. queued user input) must not break parsing.
        {
            std::wostringstream out;
            std::wistringstream in{L"\x1b[?62;1;4chello"};
            PrimaryDeviceAttributes da{out, in};

            VERIFY_IS_TRUE(da.Supports(PrimaryDeviceAttributes::Extension::Columns132));
            VERIFY_IS_TRUE(da.Supports(PrimaryDeviceAttributes::Extension::Sixel));
        }

        // Trailing VT sequence must not corrupt suffix search or result extraction.
        {
            std::wostringstream out;
            std::wistringstream in{L"\x1b[?62;6c\x1b[0m"};
            PrimaryDeviceAttributes da{out, in};

            VERIFY_IS_TRUE(da.Supports(PrimaryDeviceAttributes::Extension::SelectiveErase));
            VERIFY_IS_FALSE(da.Supports(PrimaryDeviceAttributes::Extension::Columns132));
        }

        // Empty/malformed response — should not throw, extensions remain unset.
        {
            std::wostringstream out;
            std::wistringstream in{L""};
            PrimaryDeviceAttributes da{out, in};

            VERIFY_IS_FALSE(da.Supports(PrimaryDeviceAttributes::Extension::Columns132));
        }
    }

    TEST_METHOD(VT_PrimaryDeviceAttributes_Empty)
    {
        // Empty/malformed response — should not throw, extensions remain unset.
        std::wostringstream out;
        std::wistringstream in{L""};

        PrimaryDeviceAttributes da{out, in};

        VERIFY_IS_FALSE(da.Supports(PrimaryDeviceAttributes::Extension::Columns132));
    }
};

} // namespace WSLCCLIVTSupportUnitTests
