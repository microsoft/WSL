/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VTSupport.cpp

Abstract:

    This file contains the implementation of VT sequence constructors and
    console mode helpers declared in VTSupport.h.

--*/

#include "precomp.h"
#include "VTSupport.h"

#define WSL_WINDOWS_VT_ESCAPE L"\x1b"
#define WSL_WINDOWS_VT_CSI WSL_WINDOWS_VT_ESCAPE L"["
#define WSL_WINDOWS_VT_OSC WSL_WINDOWS_VT_ESCAPE L"]"

// Two-level macro so the L prefix is pasted to the stringified token
// before adjacent-string-literal concatenation kicks in.  Without the inner
// helper, `L ## #_id_` would try to token-paste L onto a string literal,
// which is not a valid preprocessing token under MSVC's conforming mode.
#define WSL_WINDOWS_VT_WIDEN_INNER(_s_) L##_s_
#define WSL_WINDOWS_VT_WIDEN(_s_) WSL_WINDOWS_VT_WIDEN_INNER(_s_)
#define WSL_WINDOWS_VT_TEXTFORMAT(_id_) WSL_WINDOWS_VT_CSI WSL_WINDOWS_VT_WIDEN(#_id_) L"m"

namespace wsl::windows::common::vt {
namespace {
    std::wstring ExtractSequence(std::wistream& inStream, std::wstring_view prefix, std::wstring_view suffix)
    {
        (void)inStream.peek();

        static constexpr std::streamsize s_bufferSize = 1024;
        wchar_t buffer[s_bufferSize];

        std::streamsize charsRead = inStream.readsome(buffer, s_bufferSize);

        std::wstring_view resultView{buffer, static_cast<size_t>(charsRead)};

        const size_t escapeIndex = resultView.find(L'\x1b');
        if (escapeIndex == std::wstring_view::npos)
        {
            return {};
        }

        resultView = resultView.substr(escapeIndex);

        if (resultView.length() < 1 + prefix.length() || resultView.substr(1, prefix.length()) != prefix)
        {
            return {};
        }

        const std::wstring_view body = resultView.substr(1 + prefix.length());
        const size_t suffixIndex = body.find(suffix);
        if (suffixIndex == std::wstring_view::npos)
        {
            return {};
        }

        return std::wstring{body.substr(0, suffixIndex)};
    }
} // namespace

ChangeTerminalMode::ChangeTerminalMode(HANDLE console, bool cursorVisible) : m_console(console)
{
    if (!wsl::windows::common::wslutil::IsConsoleHandle(console))
    {
        m_console = nullptr;
        return;
    }

    THROW_IF_WIN32_BOOL_FALSE(GetConsoleCursorInfo(console, &m_originalCursorInfo));
    CONSOLE_CURSOR_INFO newCursorInfo = m_originalCursorInfo;
    newCursorInfo.bVisible = cursorVisible;
    THROW_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(console, &newCursorInfo));
}

ChangeTerminalMode::~ChangeTerminalMode()
{
    if (m_console)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(m_console, &m_originalCursorInfo));
    }
}

bool ChangeTerminalMode::IsConsole() const
{
    return m_console != nullptr;
}

EnableVirtualTerminal::EnableVirtualTerminal(HANDLE console, Mode mode, bool disableNewlineAutoReturn)
{
    DWORD current;
    if (!GetConsoleMode(console, &current))
    {
        LOG_LAST_ERROR_IF(GetLastError() != ERROR_INVALID_HANDLE);
        return;
    }

    if (mode == Mode::Input)
    {
        const DWORD newMode = (current & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (newMode == current)
        {
            // Already in the desired mode; nothing to restore, but VT input is
            // still enabled on the handle.
            m_vtEnabled = (current & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0;
            return;
        }

        if (SetConsoleMode(console, newMode))
        {
            m_console = console;
            m_originalMode = current;
            m_vtEnabled = true;
        }
        else
        {
            LOG_LAST_ERROR_IF(GetLastError() != ERROR_INVALID_PARAMETER);
        }
    }
    else
    {
        auto tryEnable = [&](DWORD flags) -> bool {
            const DWORD newMode = current | flags;
            if (newMode == current)
            {
                // Flags already set; no mode change needed and nothing to restore,
                // but VT processing is already enabled on the handle.
                m_vtEnabled = true;
                return true;
            }

            if (SetConsoleMode(console, newMode))
            {
                m_console = console;
                m_originalMode = current;
                m_vtEnabled = true;
                return true;
            }

            LOG_LAST_ERROR_IF(GetLastError() != ERROR_INVALID_PARAMETER);
            return false;
        };

        if (disableNewlineAutoReturn && tryEnable(ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN))
        {
            return;
        }

        tryEnable(ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

EnableVirtualTerminal::~EnableVirtualTerminal()
{
    if (m_console)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleMode(m_console, m_originalMode));
    }
}

bool EnableVirtualTerminal::IsVTEnabled() const
{
    return m_vtEnabled;
}

ConstructedSequence::ConstructedSequence()
{
    Set(m_str);
}

ConstructedSequence::ConstructedSequence(std::wstring s) : m_str(std::move(s))
{
    Set(m_str);
}

ConstructedSequence::ConstructedSequence(const ConstructedSequence& other) : m_str(other.m_str)
{
    Set(m_str);
}

ConstructedSequence& ConstructedSequence::operator=(const ConstructedSequence& other)
{
    m_str = other.m_str;
    Set(m_str);
    return *this;
}

ConstructedSequence::ConstructedSequence(ConstructedSequence&& other) noexcept : m_str(std::move(other.m_str))
{
    Set(m_str);
    other.Set(other.m_str);
}

ConstructedSequence& ConstructedSequence::operator=(ConstructedSequence&& other) noexcept
{
    m_str = std::move(other.m_str);
    Set(m_str);
    other.Set(other.m_str);
    return *this;
}

bool Sequence::IsColor() const
{
    const auto sv = m_chars;
    if (sv.size() < 2 || sv[0] != L'\x1b')
    {
        return false;
    }

    if (sv[1] == L'[')
    {
        // CSI sequence — color if final byte is 'm' (SGR)
        return sv.back() == L'm';
    }

    if (sv[1] == L']')
    {
        // OSC 8 hyperlink — treated as color-adjacent
        return sv.size() >= 3 && sv[2] == L'8';
    }

    return false;
}

void ConstructedSequence::Append(const Sequence& sequence)
{
    if (!sequence.Get().empty())
    {
        m_str += sequence.Get();
        Set(m_str);
    }
}

void ConstructedSequence::Clear()
{
    m_str.clear();
    Set(m_str);
}

ConstructedSequence Sgr(std::initializer_list<int> params)
{
    std::wostringstream result;
    result << WSL_WINDOWS_VT_CSI;
    bool first = true;
    for (const int param : params)
    {
        if (!first)
        {
            result << L';';
        }
        result << param;
        first = false;
    }
    result << L'm';
    return ConstructedSequence{std::move(result).str()};
}

PrimaryDeviceAttributes::PrimaryDeviceAttributes(std::wostream& outStream, std::wistream& inStream)
{
    try
    {
        // Best-effort: enable VT input on the real console handle so the terminal
        // sends a machine-readable DA1 response.  When stdin is redirected (e.g.
        // in unit tests that supply their own wstringstreams) this will fail, but
        // we still proceed — the caller is responsible for providing a readable
        // inStream that contains the DA1 response.
        EnableVirtualTerminal inputMode{GetStdHandle(STD_INPUT_HANDLE), EnableVirtualTerminal::Mode::Input};

        // Send DA1 Primary Device Attributes request.
        outStream << WSL_WINDOWS_VT_CSI L"0c";
        outStream.flush();

        // Response is of the form ESC[?<conformance level>;<extension>...c
        // Split returns std::vector<std::wstring> via the wstring_view template overload.
        std::wstring sequence = ExtractSequence(inStream, L"[?", L"c");
        std::vector<std::wstring> values = wsl::shared::string::Split(sequence, L';');

        if (!values.empty())
        {
            // Use wcstoul so the wchar_t digits are parsed directly without any
            // narrowing conversion.
            m_conformanceLevel = std::wcstoul(values[0].c_str(), nullptr, 10);
        }

        // m_extensions is a uint64_t bitmask; extension values >= 64 cannot be
        // represented and are silently ignored to avoid undefined behaviour from
        // an out-of-range shift.
        constexpr unsigned long c_maxExtensionBit = 63ul;
        for (size_t i = 1; i < values.size(); ++i)
        {
            const unsigned long ext = std::wcstoul(values[i].c_str(), nullptr, 10);
            if (ext <= c_maxExtensionBit)
            {
                m_extensions |= 1ull << ext;
            }
        }
    }
    CATCH_LOG();
}

bool PrimaryDeviceAttributes::Supports(Extension extension) const
{
    uint64_t extensionMask = 1ull << ToIntegral(extension);
    return (m_extensions & extensionMask) == extensionMask;
}

namespace Cursor {
    ConstructedSequence Up(int cells)
    {
        THROW_HR_IF(E_INVALIDARG, cells < 0);
        if (cells == 0)
        {
            return ConstructedSequence{};
        }
        return ConstructedSequence{std::format(WSL_WINDOWS_VT_CSI L"{}A", cells)};
    }

    ConstructedSequence Down(int cells)
    {
        THROW_HR_IF(E_INVALIDARG, cells < 0);
        if (cells == 0)
        {
            return ConstructedSequence{};
        }
        return ConstructedSequence{std::format(WSL_WINDOWS_VT_CSI L"{}B", cells)};
    }

    ConstructedSequence Forward(int cells)
    {
        THROW_HR_IF(E_INVALIDARG, cells < 0);
        if (cells == 0)
        {
            return ConstructedSequence{};
        }
        return ConstructedSequence{std::format(WSL_WINDOWS_VT_CSI L"{}C", cells)};
    }

    ConstructedSequence Backward(int cells)
    {
        THROW_HR_IF(E_INVALIDARG, cells < 0);
        if (cells == 0)
        {
            return ConstructedSequence{};
        }
        return ConstructedSequence{std::format(WSL_WINDOWS_VT_CSI L"{}D", cells)};
    }

    ConstructedSequence MoveTo(int row, int col)
    {
        THROW_HR_IF(E_INVALIDARG, row < 1 || col < 1);
        return ConstructedSequence{std::format(WSL_WINDOWS_VT_CSI L"{};{}H", row, col)};
    }

    const Sequence Home{WSL_WINDOWS_VT_CSI L"H"};
    const Sequence EnableBlink{WSL_WINDOWS_VT_CSI L"?12h"};
    const Sequence DisableBlink{WSL_WINDOWS_VT_CSI L"?12l"};
    const Sequence Show{WSL_WINDOWS_VT_CSI L"?25h"};
    const Sequence Hide{WSL_WINDOWS_VT_CSI L"?25l"};

    const Sequence BracketedPasteOn{WSL_WINDOWS_VT_CSI L"?2004h"};
    const Sequence BracketedPasteOff{WSL_WINDOWS_VT_CSI L"?2004l"};
} // namespace Cursor

namespace Format {
    const Sequence Default{WSL_WINDOWS_VT_TEXTFORMAT(0)};
    const Sequence Negative{WSL_WINDOWS_VT_TEXTFORMAT(7)};
    const Sequence Bright{WSL_WINDOWS_VT_TEXTFORMAT(1)};
    const Sequence Dim{WSL_WINDOWS_VT_TEXTFORMAT(2)};
    const Sequence Normal{WSL_WINDOWS_VT_TEXTFORMAT(22)};
    const Sequence Italic{WSL_WINDOWS_VT_TEXTFORMAT(3)};
    const Sequence NoItalic{WSL_WINDOWS_VT_TEXTFORMAT(23)};
    const Sequence Underline{WSL_WINDOWS_VT_TEXTFORMAT(4)};
    const Sequence NoUnderline{WSL_WINDOWS_VT_TEXTFORMAT(24)};

    namespace Fg {
        const Sequence Black{WSL_WINDOWS_VT_TEXTFORMAT(30)};
        const Sequence Red{WSL_WINDOWS_VT_TEXTFORMAT(31)};
        const Sequence Green{WSL_WINDOWS_VT_TEXTFORMAT(32)};
        const Sequence Yellow{WSL_WINDOWS_VT_TEXTFORMAT(33)};
        const Sequence Blue{WSL_WINDOWS_VT_TEXTFORMAT(34)};
        const Sequence Magenta{WSL_WINDOWS_VT_TEXTFORMAT(35)};
        const Sequence Cyan{WSL_WINDOWS_VT_TEXTFORMAT(36)};
        const Sequence White{WSL_WINDOWS_VT_TEXTFORMAT(37)};

        const Sequence BrightBlack{WSL_WINDOWS_VT_TEXTFORMAT(90)};
        const Sequence BrightRed{WSL_WINDOWS_VT_TEXTFORMAT(91)};
        const Sequence BrightGreen{WSL_WINDOWS_VT_TEXTFORMAT(92)};
        const Sequence BrightYellow{WSL_WINDOWS_VT_TEXTFORMAT(93)};
        const Sequence BrightBlue{WSL_WINDOWS_VT_TEXTFORMAT(94)};
        const Sequence BrightMagenta{WSL_WINDOWS_VT_TEXTFORMAT(95)};
        const Sequence BrightCyan{WSL_WINDOWS_VT_TEXTFORMAT(96)};
        const Sequence BrightWhite{WSL_WINDOWS_VT_TEXTFORMAT(97)};

        ConstructedSequence Extended(const Color& color)
        {
            std::wostringstream result;
            result << WSL_WINDOWS_VT_CSI L"38;2;" << static_cast<uint32_t>(color.R) << L';' << static_cast<uint32_t>(color.G)
                   << L';' << static_cast<uint32_t>(color.B) << L'm';
            return ConstructedSequence{std::move(result).str()};
        }
    } // namespace Fg

    namespace Bg {
        const Sequence Black{WSL_WINDOWS_VT_TEXTFORMAT(40)};
        const Sequence Red{WSL_WINDOWS_VT_TEXTFORMAT(41)};
        const Sequence Green{WSL_WINDOWS_VT_TEXTFORMAT(42)};
        const Sequence Yellow{WSL_WINDOWS_VT_TEXTFORMAT(43)};
        const Sequence Blue{WSL_WINDOWS_VT_TEXTFORMAT(44)};
        const Sequence Magenta{WSL_WINDOWS_VT_TEXTFORMAT(45)};
        const Sequence Cyan{WSL_WINDOWS_VT_TEXTFORMAT(46)};
        const Sequence White{WSL_WINDOWS_VT_TEXTFORMAT(47)};

        const Sequence BrightBlack{WSL_WINDOWS_VT_TEXTFORMAT(100)};
        const Sequence BrightRed{WSL_WINDOWS_VT_TEXTFORMAT(101)};
        const Sequence BrightGreen{WSL_WINDOWS_VT_TEXTFORMAT(102)};
        const Sequence BrightYellow{WSL_WINDOWS_VT_TEXTFORMAT(103)};
        const Sequence BrightBlue{WSL_WINDOWS_VT_TEXTFORMAT(104)};
        const Sequence BrightMagenta{WSL_WINDOWS_VT_TEXTFORMAT(105)};
        const Sequence BrightCyan{WSL_WINDOWS_VT_TEXTFORMAT(106)};
        const Sequence BrightWhite{WSL_WINDOWS_VT_TEXTFORMAT(107)};

        ConstructedSequence Extended(const Color& color)
        {
            std::wostringstream result;
            result << WSL_WINDOWS_VT_CSI L"48;2;" << static_cast<uint32_t>(color.R) << L';' << static_cast<uint32_t>(color.G)
                   << L';' << static_cast<uint32_t>(color.B) << L'm';
            return ConstructedSequence{std::move(result).str()};
        }
    } // namespace Bg

    ConstructedSequence Hyperlink(const std::wstring& text, const std::wstring& ref)
    {
        std::wostringstream result;
        result << WSL_WINDOWS_VT_OSC L"8;;" << ref << WSL_WINDOWS_VT_ESCAPE << L"\\" << text << WSL_WINDOWS_VT_OSC << L"8;;"
               << WSL_WINDOWS_VT_ESCAPE << L"\\";
        return ConstructedSequence{std::move(result).str()};
    }
} // namespace Format

namespace Erase {
    const Sequence LineForward{WSL_WINDOWS_VT_CSI L"K"};
    const Sequence LineBackward{WSL_WINDOWS_VT_CSI L"1K"};
    const Sequence LineEntirely{WSL_WINDOWS_VT_CSI L"2K"};
    const Sequence ScreenForward{WSL_WINDOWS_VT_CSI L"J"};
    const Sequence ScreenBackward{WSL_WINDOWS_VT_CSI L"1J"};
    const Sequence ScreenEntirely{WSL_WINDOWS_VT_CSI L"2J"};
} // namespace Erase

namespace Progress {
    ConstructedSequence Construct(State state, std::optional<uint32_t> percentage)
    {
        // See https://conemu.github.io/en/AnsiEscapeCodes.html#ConEmu_specific_OSC

        THROW_HR_IF(E_BOUNDS, percentage.has_value() && percentage.value() > 100u);

        // Workaround some quirks in the Windows Terminal implementation of the progress OSC sequence
        switch (state)
        {
        case State::None:
        case State::Indeterminate:
            // Windows Terminal does not recognize the OSC sequence if the progress value is left out.
            // As a workaround, we can specify an arbitrary value since it does not matter for None and Indeterminate states.
            percentage = percentage.value_or(0);
            break;
        case State::Normal:
        case State::Error:
        case State::Paused:
            // Windows Terminal does not support switching progress states without also setting a progress value at the same time,
            // so we disallow this case for now.
            THROW_HR_IF(E_INVALIDARG, !percentage.has_value());
            break;
        }

        int stateId;
        switch (state)
        {
        case State::None:
            stateId = 0;
            break;
        case State::Indeterminate:
            stateId = 3;
            break;
        case State::Normal:
            stateId = 1;
            break;
        case State::Error:
            stateId = 2;
            break;
        case State::Paused:
            stateId = 4;
            break;
        default:
            THROW_HR(E_UNEXPECTED);
        }

        std::wostringstream result;
        result << WSL_WINDOWS_VT_OSC L"9;4;" << stateId << L";";
        if (percentage.has_value())
        {
            result << percentage.value();
        }
        result << WSL_WINDOWS_VT_ESCAPE << L"\\";
        return ConstructedSequence{std::move(result).str()};
    }
} // namespace Progress

std::wstring operator+(const Sequence& lhs, const Sequence& rhs)
{
    std::wstring out;
    out.reserve(lhs.Get().size() + rhs.Get().size());
    out.append(lhs.Get()).append(rhs.Get());
    return out;
}

std::wstring operator+(const Sequence& lhs, const std::wstring& rhs)
{
    return std::wstring{lhs.Get()} + rhs;
}

std::wstring operator+(const std::wstring& lhs, const Sequence& rhs)
{
    return lhs + std::wstring{rhs.Get()};
}

std::wstring operator+(const Sequence& lhs, const wchar_t* rhs)
{
    return std::wstring{lhs.Get()} + rhs;
}

std::wstring operator+(const wchar_t* lhs, const Sequence& rhs)
{
    return lhs + std::wstring{rhs.Get()};
}

std::wstring& operator+=(std::wstring& lhs, const Sequence& rhs)
{
    lhs.append(rhs.Get());
    return lhs;
}
} // namespace wsl::windows::common::vt
