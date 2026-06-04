/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VTSupport.h

Abstract:

    This file contains VT (Virtual Terminal) sequence constants, construction
    helpers, and console mode RAII wrappers for use in Windows WSL components.

--*/

#pragma once

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include "wslutil.h"

// The escape character that begins all VT sequences
#define WSL_WINDOWS_VT_ESCAPE     "\x1b"

namespace wsl::windows::common::vt {

    // Get the integral value for an enum.
    template <typename E>
    constexpr inline std::enable_if_t<std::is_enum_v<E>, std::underlying_type_t<E>> ToIntegral(E e)
    {
        return static_cast<std::underlying_type_t<E>>(e);
    }

    // Get the enum value for an integral.
    template <typename E>
    constexpr inline std::enable_if_t<std::is_enum_v<E>, E> ToEnum(std::underlying_type_t<E> ut)
    {
        return static_cast<E>(ut);
    }

    // RAII helper that changes cursor visibility on a console handle and restores
    // the original cursor info on destruction. No-op if the handle is not a console.
    class ChangeTerminalMode
    {
    public:
        NON_COPYABLE(ChangeTerminalMode);
        NON_MOVABLE(ChangeTerminalMode);

        ChangeTerminalMode(HANDLE console, bool cursorVisible) : m_console(console)
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

        ~ChangeTerminalMode()
        {
            if (m_console)
            {
                LOG_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(m_console, &m_originalCursorInfo));
            }
        }

        bool IsConsole() const
        {
            return m_console != nullptr;
        }

    private:
        HANDLE m_console{};
        CONSOLE_CURSOR_INFO m_originalCursorInfo{};
    };

    // RAII helper that enables VT processing on a console handle and restores the
    // original mode on destruction. No-op if the handle is not a console.
    //
    // Output mode (STD_OUTPUT_HANDLE): sets ENABLE_VIRTUAL_TERMINAL_PROCESSING,
    // optionally DISABLE_NEWLINE_AUTO_RETURN (best-effort, falls back without it).
    //
    // Input mode (STD_INPUT_HANDLE): sets ENABLE_VIRTUAL_TERMINAL_INPUT and
    // ENABLE_EXTENDED_FLAGS, clears ENABLE_LINE_INPUT and ENABLE_ECHO_INPUT.
    class EnableVirtualTerminal
    {
    public:
        NON_COPYABLE(EnableVirtualTerminal);
        NON_MOVABLE(EnableVirtualTerminal);

        enum class Mode
        {
            Output,
            Input,
        };

        explicit EnableVirtualTerminal(HANDLE console, Mode mode = Mode::Output, bool disableNewlineAutoReturn = false)
        {
            DWORD current;
            if (!GetConsoleMode(console, &current))
            {
                LOG_LAST_ERROR_IF(GetLastError() != ERROR_INVALID_HANDLE);
                return;
            }

            if (mode == Mode::Input)
            {
                const DWORD newMode =
                    (current & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT;
                if (SetConsoleMode(console, newMode))
                {
                    m_console = console;
                    m_originalMode = current;
                }
                else
                {
                    LOG_LAST_ERROR_IF(GetLastError() != ERROR_INVALID_PARAMETER);
                }
            }
            else
            {
                const DWORD preferredFlags =
                    ENABLE_VIRTUAL_TERMINAL_PROCESSING | (disableNewlineAutoReturn ? DISABLE_NEWLINE_AUTO_RETURN : 0);

                for (DWORD flags : {preferredFlags, static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_PROCESSING)})
                {
                    const DWORD newMode = current | flags;
                    if (newMode == current)
                    {
                        return;
                    }
                    if (SetConsoleMode(console, newMode))
                    {
                        m_console = console;
                        m_originalMode = current;
                        return;
                    }
                    LOG_LAST_ERROR_IF(GetLastError() != ERROR_INVALID_PARAMETER);
                }
            }
        }

        ~EnableVirtualTerminal()
        {
            if (m_console)
            {
                LOG_IF_WIN32_BOOL_FALSE(SetConsoleMode(m_console, m_originalMode));
            }
        }

        bool IsVTEnabled() const
        {
            return m_console != nullptr;
        }

    private:
        HANDLE m_console = nullptr;
        DWORD m_originalMode = 0;
    };

    // VT escape sequences are pure ASCII byte sequences (0x00-0x7F), so all sequences
    // are stored and manipulated as narrow strings (std::string / std::string_view).
    // Widening to std::wstring happens only at the stream output boundary; see
    // operator<<(std::wostream&, const Sequence&) below.

    // The base for all VT sequences.
    struct Sequence
    {
        constexpr Sequence() = default;
        explicit constexpr Sequence(std::string_view c) : m_chars(c) {}

        std::string_view Get() const { return m_chars; }

    protected:
        void Set(const std::string& s) { m_chars = s; }

    private:
        std::string_view m_chars;
    };

    // A VT sequence that is constructed at runtime.
    struct ConstructedSequence : public Sequence
    {
        ConstructedSequence() { Set(m_str); }
        explicit ConstructedSequence(std::string s) : m_str(std::move(s)) { Set(m_str); }

        ConstructedSequence(const ConstructedSequence& other) : m_str(other.m_str) { Set(m_str); }
        ConstructedSequence& operator=(const ConstructedSequence& other) { m_str = other.m_str; Set(m_str); return *this; }

        ConstructedSequence(ConstructedSequence&& other) noexcept : m_str(std::move(other.m_str)) { Set(m_str); }
        ConstructedSequence& operator=(ConstructedSequence&& other) noexcept { m_str = std::move(other.m_str); Set(m_str); return *this; }

        void Append(const Sequence& sequence);

        void Clear();

    private:
        std::string m_str;
    };

    // Constructs a single SGR (Select Graphic Rendition) sequence with one or more
    // semicolon-separated parameters. e.g. Sgr({1, 31}) produces "\x1b[1;31m".
    // Prefer named constants in the Format namespace for single-parameter sequences;
    // use this only when a multi-parameter form is required to match specific terminal
    // output exactly (e.g. a shell PS1 that emits combined bold+color in one sequence).
    ConstructedSequence Sgr(std::initializer_list<int> params);

    // Below are mapped to the sequences described here:
    // https://docs.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences

    // Contains the response to a DA1 (Primary Device Attributes) request.
    struct PrimaryDeviceAttributes
    {
        // Queries the device attributes on creation.
        PrimaryDeviceAttributes(std::ostream& outStream, std::istream& inStream);

        // The extensions that a device may support.
        enum class Extension
        {
            Columns132 = 1,
            PrinterPort = 2,
            Sixel = 4,
            SelectiveErase = 6,
            SoftCharacterSet = 7,
            UserDefinedKeys = 8,
            NationalReplacementCharacterSets = 9,
            SoftCharacterSet2 = 12,
            EightBitInterface = 14,
            TechnicalCharacterSet = 15,
            WindowingCapability = 18,
            HorizontalScrolling = 21,
            ColorText = 22,
            Greek = 23,
            Turkish = 24,
            RectangularAreaOperations = 28,
            TextMacros = 32,
            ISO_Latin2CharacterSet = 42,
            PC_Term = 44,
            SoftKeyMap = 45,
            ASCII_Emulation = 46,
        };

        // Determines if the given extension is supported.
        bool Supports(Extension extension) const;

    private:
        uint32_t m_conformanceLevel = 0;
        uint64_t m_extensions = 0;
    };

    // Cursor movement, visibility, and input mode sequences.
    namespace Cursor
    {
        // Move cursor N cells in the given direction.
        ConstructedSequence Up(int cells);
        ConstructedSequence Down(int cells);
        ConstructedSequence Forward(int cells);
        ConstructedSequence Backward(int cells);

        // Move cursor to an absolute position (1-based row and column).
        ConstructedSequence MoveTo(int row, int col);

        // Move cursor to the top-left corner of the screen.
        extern const Sequence Home;

        // Cursor visibility.
        extern const Sequence EnableBlink;
        extern const Sequence DisableBlink;
        extern const Sequence Show;
        extern const Sequence Hide;

        // Bracketed paste mode causes the terminal to wrap pasted text in escape sequences
        // so the application can distinguish typed input from pasted input.
        // See https://cirw.in/blog/bracketed-paste
        extern const Sequence BracketedPasteOn;
        extern const Sequence BracketedPasteOff;
    }

    // Text formatting (color, weight, style) sequences.
    namespace Format
    {
        extern const Sequence Default;
        extern const Sequence Negative;

        // Intensity attributes. Normal cancels both Bright and Dim (SGR 22).
        extern const Sequence Bright;
        extern const Sequence Dim;
        extern const Sequence Normal;

        extern const Sequence Italic;
        extern const Sequence NoItalic;
        extern const Sequence Underline;
        extern const Sequence NoUnderline;

        // A color, used in constructed sequences.
        struct Color
        {
            uint8_t R;
            uint8_t G;
            uint8_t B;

            static Color GetAccentColor();
        };

        namespace Fg
        {
            // Standard foreground colors using SGR 30-37.
            extern const Sequence Black;
            extern const Sequence Red;
            extern const Sequence Green;
            extern const Sequence Yellow;
            extern const Sequence Blue;
            extern const Sequence Magenta;
            extern const Sequence Cyan;
            extern const Sequence White;

            // High-intensity ("bright") foreground colors using SGR 90-97.
            // These are distinct from SGR 1;3x (bold + standard color), which is
            // a different byte sequence even though terminals often render them identically.
            extern const Sequence BrightBlack;   // Typically rendered as dark gray.
            extern const Sequence BrightRed;
            extern const Sequence BrightGreen;
            extern const Sequence BrightYellow;
            extern const Sequence BrightBlue;
            extern const Sequence BrightMagenta;
            extern const Sequence BrightCyan;
            extern const Sequence BrightWhite;

            ConstructedSequence Extended(const Color& color);
        }

        namespace Bg
        {
            // Standard background colors using SGR 40-47.
            extern const Sequence Black;
            extern const Sequence Red;
            extern const Sequence Green;
            extern const Sequence Yellow;
            extern const Sequence Blue;
            extern const Sequence Magenta;
            extern const Sequence Cyan;
            extern const Sequence White;

            // High-intensity ("bright") background colors using SGR 100-107.
            extern const Sequence BrightBlack;   // Typically rendered as dark gray.
            extern const Sequence BrightRed;
            extern const Sequence BrightGreen;
            extern const Sequence BrightYellow;
            extern const Sequence BrightBlue;
            extern const Sequence BrightMagenta;
            extern const Sequence BrightCyan;
            extern const Sequence BrightWhite;

            ConstructedSequence Extended(const Color& color);
        }

        ConstructedSequence Hyperlink(const std::string& text, const std::string& ref);

        // Constructs a single SGR sequence with one or more semicolon-separated parameters.
        // e.g. Sgr({1, 31}) produces "\x1b[1;31m" — bold + red in one sequence.
        // Prefer named constants for single-parameter sequences; use this only when
        // a multi-parameter form is required to match specific terminal output exactly.
        ConstructedSequence Sgr(std::initializer_list<int> params);
    }

    // Line and screen erasure sequences.
    namespace Erase
    {
        extern const Sequence LineForward;
        extern const Sequence LineBackward;
        extern const Sequence LineEntirely;
        extern const Sequence ScreenForward;
        extern const Sequence ScreenBackward;
        extern const Sequence ScreenEntirely;
    }

    namespace Progress
    {
        enum class State
        {
            None,
            Indeterminate,
            Normal,
            Paused,
            Error
        };

        ConstructedSequence Construct(State state, std::optional<uint32_t> percentage = std::nullopt);
    } // namespace Progress

    // operator<< for stream output.
    // Widens the narrow sequence bytes (all ASCII) into a wide string for wostream output.
    inline std::wostream& operator<<(std::wostream& o, const Sequence& s)
    {
        const auto sv = s.Get();
        return (o << std::wstring(sv.begin(), sv.end()));
    }

    inline std::ostream& operator<<(std::ostream& o, const Sequence& s)
    {
        return (o << s.Get());
    }

    // operator+ overloads for direct std::string / std::wstring concatenation with sequences.
    // These allow sequences to be combined with string literals and std::string without
    // manually calling .Get() or wrapping in std::string{...}.

    inline std::string operator+(const Sequence& lhs, const Sequence& rhs)
    {
        return std::string{ lhs.Get() } + std::string{ rhs.Get() };
    }

    inline std::string operator+(const Sequence& lhs, const std::string& rhs)
    {
        return std::string{ lhs.Get() } + rhs;
    }

    inline std::string operator+(const std::string& lhs, const Sequence& rhs)
    {
        return lhs + std::string{ rhs.Get() };
    }

    inline std::string operator+(const Sequence& lhs, const char* rhs)
    {
        return std::string{ lhs.Get() } + rhs;
    }

    inline std::string operator+(const char* lhs, const Sequence& rhs)
    {
        return lhs + std::string{ rhs.Get() };
    }

    // Wide string variants — sequences are ASCII so widening is lossless.
    inline std::wstring operator+(const Sequence& lhs, const std::wstring& rhs)
    {
        const auto sv = lhs.Get();
        return std::wstring(sv.begin(), sv.end()) + rhs;
    }

    inline std::wstring operator+(const std::wstring& lhs, const Sequence& rhs)
    {
        const auto sv = rhs.Get();
        return lhs + std::wstring(sv.begin(), sv.end());
    }

    // operator== overloads so any Sequence-derived type can be compared directly against
    // string literals and std::string_view without calling .Get() at every call site.
    // Templated to cover both Sequence and ConstructedSequence without separate overloads.

    template<typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
    inline bool operator==(const T& lhs, std::string_view rhs)
    {
        return lhs.Get() == rhs;
    }

    template<typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
    inline bool operator==(std::string_view lhs, const T& rhs)
    {
        return lhs == rhs.Get();
    }

    template<typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
    inline bool operator==(const T& lhs, const char* rhs)
    {
        return lhs.Get() == rhs;
    }

    template<typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
    inline bool operator==(const char* lhs, const T& rhs)
    {
        return lhs == rhs.Get();
    }

    // Widens a Sequence's ASCII bytes into a std::wstring.
    // Use when building a wide string buffer that mixes VT sequences with wide content.
    inline std::wstring ToWide(const Sequence& s)
    {
        const auto sv = s.Get();
        return std::wstring(sv.begin(), sv.end());
    }

} // namespace wsl::windows::common::vt
