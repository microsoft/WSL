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

namespace wsl::windows::common::vt
{
    namespace
    {
        // Extracts a VT sequence, expected one of the form ESCAPE + prefix + result + suffix, returning the result part.
        std::string ExtractSequence(std::istream& inStream, std::string_view prefix, std::string_view suffix)
        {
            // Force discovery of available input
            std::ignore = inStream.peek();

            static constexpr std::streamsize s_bufferSize = 1024;
            char buffer[s_bufferSize];
            std::streamsize bytesRead = inStream.readsome(buffer, s_bufferSize);
            THROW_HR_IF(E_UNEXPECTED, bytesRead >= s_bufferSize);

            std::string_view resultView{ buffer, static_cast<size_t>(bytesRead) };
            size_t escapeIndex = resultView.find(WSL_WINDOWS_VT_ESCAPE[0]);
            if (escapeIndex == std::string_view::npos)
            {
                return {};
            }

            resultView = resultView.substr(escapeIndex);
            size_t overheadLength = 1 + prefix.length() + suffix.length();
            if (resultView.length() <= overheadLength ||
                resultView.substr(1, prefix.length()) != prefix ||
                resultView.substr(resultView.length() - suffix.length()) != suffix)
            {
                return {};
            }

            return std::string{ resultView.substr(1 + prefix.length(), resultView.length() - overheadLength) };
        }
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

} // namespace wsl::windows::common::vt

// The beginning of a Control Sequence Introducer
#define WSL_WINDOWS_VT_CSI          WSL_WINDOWS_VT_ESCAPE "["

// The beginning of an Operating system command
#define WSL_WINDOWS_VT_OSC          WSL_WINDOWS_VT_ESCAPE "]"

// Define a text formatting sequence with an integer id
#define WSL_WINDOWS_VT_TEXTFORMAT(_id_)   WSL_WINDOWS_VT_CSI #_id_ "m"

namespace wsl::windows::common::vt
{
    ConstructedSequence Sgr(std::initializer_list<int> params)
    {
        std::ostringstream result;
        result << WSL_WINDOWS_VT_CSI;
        bool first = true;
        for (const int param : params)
        {
            if (!first)
            {
                result << ';';
            }
            result << param;
            first = false;
        }
        result << 'm';
        return ConstructedSequence{ std::move(result).str() };
    }

    PrimaryDeviceAttributes::PrimaryDeviceAttributes(std::ostream& outStream, std::istream& inStream)
    {
        try
        {
            EnableVirtualTerminal inputMode{ GetStdHandle(STD_INPUT_HANDLE), EnableVirtualTerminal::Mode::Input };
            if (!inputMode.IsVTEnabled())
            {
                return;
            }

            // Send DA1 Primary Device Attributes request
            outStream << WSL_WINDOWS_VT_CSI << "0c";
            outStream.flush();

            // Response is of the form WSL_WINDOWS_VT_CSI ? <conformance level> ; (<extension number> ;)* c
            std::string sequence = ExtractSequence(inStream, "[?", "c");
            std::vector<std::string> values = wsl::shared::string::Split(sequence, ';');

            if (!values.empty())
            {
                m_conformanceLevel = std::stoul(values[0]);
            }

            for (size_t i = 1; i < values.size(); ++i)
            {
                m_extensions |= 1ull << std::stoul(values[i]);
            }
        }
        CATCH_LOG();
    }

    bool PrimaryDeviceAttributes::Supports(Extension extension) const
    {
        uint64_t extensionMask = 1ull << ToIntegral(extension);
        return (m_extensions & extensionMask) == extensionMask;
    }

    namespace Cursor
    {
        ConstructedSequence Up(int cells)
        {
            THROW_HR_IF(E_INVALIDARG, cells < 0);
            std::ostringstream result;
            result << WSL_WINDOWS_VT_CSI << cells << 'A';
            return ConstructedSequence{ std::move(result).str() };
        }

        ConstructedSequence Down(int cells)
        {
            THROW_HR_IF(E_INVALIDARG, cells < 0);
            std::ostringstream result;
            result << WSL_WINDOWS_VT_CSI << cells << 'B';
            return ConstructedSequence{ std::move(result).str() };
        }

        ConstructedSequence Forward(int cells)
        {
            THROW_HR_IF(E_INVALIDARG, cells < 0);
            std::ostringstream result;
            result << WSL_WINDOWS_VT_CSI << cells << 'C';
            return ConstructedSequence{ std::move(result).str() };
        }

        ConstructedSequence Backward(int cells)
        {
            THROW_HR_IF(E_INVALIDARG, cells < 0);
            std::ostringstream result;
            result << WSL_WINDOWS_VT_CSI << cells << 'D';
            return ConstructedSequence{ std::move(result).str() };
        }

        ConstructedSequence MoveTo(int row, int col)
        {
            THROW_HR_IF(E_INVALIDARG, row < 1 || col < 1);
            std::ostringstream result;
            result << WSL_WINDOWS_VT_CSI << row << ';' << col << 'H';
            return ConstructedSequence{ std::move(result).str() };
        }

        const Sequence Home{ WSL_WINDOWS_VT_CSI "H" };
        const Sequence SavePos{ WSL_WINDOWS_VT_CSI "s" };
        const Sequence RestorePos{ WSL_WINDOWS_VT_CSI "u" };

        const Sequence EnableBlink{ WSL_WINDOWS_VT_CSI "?12h" };
        const Sequence DisableBlink{ WSL_WINDOWS_VT_CSI "?12l" };
        const Sequence Show{ WSL_WINDOWS_VT_CSI "?25h" };
        const Sequence Hide{ WSL_WINDOWS_VT_CSI "?25l" };

        const Sequence BracketedPasteOn{ WSL_WINDOWS_VT_CSI "?2004h" };
        const Sequence BracketedPasteOff{ WSL_WINDOWS_VT_CSI "?2004l" };
    }

    namespace Format
    {
        const Sequence Default        { WSL_WINDOWS_VT_TEXTFORMAT(0) };
        const Sequence Negative       { WSL_WINDOWS_VT_TEXTFORMAT(7) };
        const Sequence Bright         { WSL_WINDOWS_VT_TEXTFORMAT(1) };
        const Sequence Dim            { WSL_WINDOWS_VT_TEXTFORMAT(2) };
        const Sequence Normal         { WSL_WINDOWS_VT_TEXTFORMAT(22) };
        const Sequence Italic         { WSL_WINDOWS_VT_TEXTFORMAT(3) };
        const Sequence NoItalic       { WSL_WINDOWS_VT_TEXTFORMAT(23) };
        const Sequence Underline      { WSL_WINDOWS_VT_TEXTFORMAT(4) };
        const Sequence NoUnderline    { WSL_WINDOWS_VT_TEXTFORMAT(24) };

        namespace Fg
        {
            const Sequence Black  { WSL_WINDOWS_VT_TEXTFORMAT(30) };
            const Sequence Red    { WSL_WINDOWS_VT_TEXTFORMAT(31) };
            const Sequence Green  { WSL_WINDOWS_VT_TEXTFORMAT(32) };
            const Sequence Yellow { WSL_WINDOWS_VT_TEXTFORMAT(33) };
            const Sequence Blue   { WSL_WINDOWS_VT_TEXTFORMAT(34) };
            const Sequence Magenta{ WSL_WINDOWS_VT_TEXTFORMAT(35) };
            const Sequence Cyan   { WSL_WINDOWS_VT_TEXTFORMAT(36) };
            const Sequence White  { WSL_WINDOWS_VT_TEXTFORMAT(37) };

            const Sequence BrightBlack  { WSL_WINDOWS_VT_TEXTFORMAT(90) };
            const Sequence BrightRed    { WSL_WINDOWS_VT_TEXTFORMAT(91) };
            const Sequence BrightGreen  { WSL_WINDOWS_VT_TEXTFORMAT(92) };
            const Sequence BrightYellow { WSL_WINDOWS_VT_TEXTFORMAT(93) };
            const Sequence BrightBlue   { WSL_WINDOWS_VT_TEXTFORMAT(94) };
            const Sequence BrightMagenta{ WSL_WINDOWS_VT_TEXTFORMAT(95) };
            const Sequence BrightCyan   { WSL_WINDOWS_VT_TEXTFORMAT(96) };
            const Sequence BrightWhite  { WSL_WINDOWS_VT_TEXTFORMAT(97) };

            ConstructedSequence Extended(const Color& color)
            {
                std::ostringstream result;
                result << WSL_WINDOWS_VT_CSI "38;2;" << static_cast<uint32_t>(color.R) << ';' << static_cast<uint32_t>(color.G) << ';' << static_cast<uint32_t>(color.B) << 'm';
                return ConstructedSequence{ std::move(result).str() };
            }
        }

        namespace Bg
        {
            const Sequence Black  { WSL_WINDOWS_VT_TEXTFORMAT(40) };
            const Sequence Red    { WSL_WINDOWS_VT_TEXTFORMAT(41) };
            const Sequence Green  { WSL_WINDOWS_VT_TEXTFORMAT(42) };
            const Sequence Yellow { WSL_WINDOWS_VT_TEXTFORMAT(43) };
            const Sequence Blue   { WSL_WINDOWS_VT_TEXTFORMAT(44) };
            const Sequence Magenta{ WSL_WINDOWS_VT_TEXTFORMAT(45) };
            const Sequence Cyan   { WSL_WINDOWS_VT_TEXTFORMAT(46) };
            const Sequence White  { WSL_WINDOWS_VT_TEXTFORMAT(47) };

            const Sequence BrightBlack  { WSL_WINDOWS_VT_TEXTFORMAT(100) };
            const Sequence BrightRed    { WSL_WINDOWS_VT_TEXTFORMAT(101) };
            const Sequence BrightGreen  { WSL_WINDOWS_VT_TEXTFORMAT(102) };
            const Sequence BrightYellow { WSL_WINDOWS_VT_TEXTFORMAT(103) };
            const Sequence BrightBlue   { WSL_WINDOWS_VT_TEXTFORMAT(104) };
            const Sequence BrightMagenta{ WSL_WINDOWS_VT_TEXTFORMAT(105) };
            const Sequence BrightCyan   { WSL_WINDOWS_VT_TEXTFORMAT(106) };
            const Sequence BrightWhite  { WSL_WINDOWS_VT_TEXTFORMAT(107) };

            ConstructedSequence Extended(const Color& color)
            {
                std::ostringstream result;
                result << WSL_WINDOWS_VT_CSI "48;2;" << static_cast<uint32_t>(color.R) << ';' << static_cast<uint32_t>(color.G) << ';' << static_cast<uint32_t>(color.B) << 'm';
                return ConstructedSequence{ std::move(result).str() };
            }
        }

        ConstructedSequence Hyperlink(const std::string& text, const std::string& ref)
        {
            std::ostringstream result;
            result << WSL_WINDOWS_VT_OSC "8;;" << ref << WSL_WINDOWS_VT_ESCAPE << "\\" << text << WSL_WINDOWS_VT_OSC << "8;;" << WSL_WINDOWS_VT_ESCAPE << "\\";
            return ConstructedSequence{ std::move(result).str() };
        }
    }

    namespace Erase
    {
        const Sequence LineForward{ WSL_WINDOWS_VT_CSI "0K" };
        const Sequence LineBackward{ WSL_WINDOWS_VT_CSI "1K" };
        const Sequence LineEntirely{ WSL_WINDOWS_VT_CSI "2K" };
        const Sequence ScreenForward{ WSL_WINDOWS_VT_CSI "J" };
        const Sequence ScreenBackward{ WSL_WINDOWS_VT_CSI "1J" };
        const Sequence ScreenEntirely{ WSL_WINDOWS_VT_CSI "2J" };
    }

    namespace Progress
    {
        ConstructedSequence Construct(State state, std::optional<uint32_t> percentage)
        {
            // See https://conemu.github.io/en/AnsiEscapeCodes.html#ConEmu_specific_OSC

            THROW_HR_IF(E_BOUNDS, percentage.has_value() && percentage > 100u);

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

            std::ostringstream result;
            result << WSL_WINDOWS_VT_OSC "9;4;" << stateId << ";";
            if (percentage.has_value())
            {
                result << percentage.value();
            }
            result << WSL_WINDOWS_VT_ESCAPE << "\\";
            return ConstructedSequence{ std::move(result).str() };
        }
    } // namespace Progress
 } // namespace wsl::windows::common::vt
