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
#include <format>
#include <initializer_list>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include "wslutil.h"

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

    ChangeTerminalMode(HANDLE console, bool cursorVisible);
    ~ChangeTerminalMode();

    bool IsConsole() const;

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

    explicit EnableVirtualTerminal(HANDLE console, Mode mode = Mode::Output, bool disableNewlineAutoReturn = false);
    ~EnableVirtualTerminal();

    // Returns true if VT processing is currently enabled on the console handle,
    // whether this instance enabled it or it was already enabled at construction.
    // This is the correct gate for "should we emit VT escape sequences?".
    // It is independent of whether the destructor will restore the prior mode
    // (that ownership is tracked separately by m_console).
    bool IsVTEnabled() const;

private:
    HANDLE m_console = nullptr; // non-null only when this instance must restore on destruction
    DWORD m_originalMode = 0;
    bool m_vtEnabled = false; // true when VT processing is active on the console handle
};

// VT escape sequences are pure ASCII byte sequences (0x00-0x7F), but the
// Windows WSL components are wide-string throughout (WriteConsoleW, wostream,
// std::wstring buffers).  Sequences are therefore stored as std::wstring /
// std::wstring_view so they compose directly with the surrounding wide-string
// code with no per-call widening.  Use the std::formatter specialization below
// for std::wformat output.

// The base for all VT sequences.
struct Sequence
{
    constexpr Sequence() = default;
    explicit constexpr Sequence(std::wstring_view c) : m_chars(c)
    {
    }

    // Prevent construction from a std::wstring (lvalue or rvalue): std::wstring is
    // implicitly convertible to std::wstring_view, so without this guard
    // Sequence(someString) would compile but leave m_chars dangling once the string
    // is destroyed.  Use ConstructedSequence for runtime / owned sequences.
    // A constrained template (rather than named overloads) avoids making wchar_t[]
    // literals ambiguous between the deleted and wstring_view constructors.
    template <typename T>
        requires std::is_same_v<std::remove_cvref_t<T>, std::wstring>
    explicit Sequence(T&&) = delete;

    std::wstring_view Get() const
    {
        return m_chars;
    }

    // Returns true if this is a color or formatting sequence (SGR or OSC 8 hyperlink)
    // that should be suppressed when --no-color is set.
    bool IsColor() const;

protected:
    void Set(const std::wstring& s)
    {
        m_chars = s;
    }

private:
    std::wstring_view m_chars;
};

// A VT sequence that is constructed at runtime.
struct ConstructedSequence : public Sequence
{
    ConstructedSequence();
    explicit ConstructedSequence(std::wstring s);

    ConstructedSequence(const ConstructedSequence& other);
    ConstructedSequence& operator=(const ConstructedSequence& other);

    ConstructedSequence(ConstructedSequence&& other) noexcept;
    ConstructedSequence& operator=(ConstructedSequence&& other) noexcept;

    void Append(const Sequence& sequence);
    void Clear();

private:
    std::wstring m_str;
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
    // Both streams must be opened in _O_U8TEXT mode (or equivalent wide mode).
    // outStream receives the DA1 request; inStream provides the terminal's response.
    PrimaryDeviceAttributes(std::wostream& outStream, std::wistream& inStream);

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
namespace Cursor {
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
} // namespace Cursor

// Text formatting (color, weight, style) sequences.
namespace Format {
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
    };

    namespace Fg {
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
        extern const Sequence BrightBlack; // Typically rendered as dark gray.
        extern const Sequence BrightRed;
        extern const Sequence BrightGreen;
        extern const Sequence BrightYellow;
        extern const Sequence BrightBlue;
        extern const Sequence BrightMagenta;
        extern const Sequence BrightCyan;
        extern const Sequence BrightWhite;

        ConstructedSequence Extended(const Color& color);
    } // namespace Fg

    namespace Bg {
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
        extern const Sequence BrightBlack; // Typically rendered as dark gray.
        extern const Sequence BrightRed;
        extern const Sequence BrightGreen;
        extern const Sequence BrightYellow;
        extern const Sequence BrightBlue;
        extern const Sequence BrightMagenta;
        extern const Sequence BrightCyan;
        extern const Sequence BrightWhite;

        ConstructedSequence Extended(const Color& color);
    } // namespace Bg

    ConstructedSequence Hyperlink(const std::wstring& text, const std::wstring& ref);
} // namespace Format

// Line and screen erasure sequences.
namespace Erase {
    extern const Sequence LineForward;
    extern const Sequence LineBackward;
    extern const Sequence LineEntirely;
    extern const Sequence ScreenForward;
    extern const Sequence ScreenBackward;
    extern const Sequence ScreenEntirely;
} // namespace Erase

namespace Progress {
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

// operator+ overloads for combining sequences with wide strings.
std::wstring operator+(const Sequence& lhs, const Sequence& rhs);
std::wstring operator+(const Sequence& lhs, const std::wstring& rhs);
std::wstring operator+(const std::wstring& lhs, const Sequence& rhs);
std::wstring operator+(const Sequence& lhs, const wchar_t* rhs);
std::wstring operator+(const wchar_t* lhs, const Sequence& rhs);

// operator== overloads for comparing sequences against string literals.
template <typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
inline bool operator==(const T& lhs, std::wstring_view rhs)
{
    return lhs.Get() == rhs;
}

template <typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
inline bool operator==(std::wstring_view lhs, const T& rhs)
{
    return lhs == rhs.Get();
}

template <typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
inline bool operator==(const T& lhs, const wchar_t* rhs)
{
    return lhs.Get() == rhs;
}

template <typename T, typename = std::enable_if_t<std::is_base_of<Sequence, T>::value>>
inline bool operator==(const wchar_t* lhs, const T& rhs)
{
    return lhs == rhs.Get();
}

// In-place wide string append.
std::wstring& operator+=(std::wstring& lhs, const Sequence& rhs);

} // namespace wsl::windows::common::vt

// std::formatter specializations, must be outside namespace.
template <>
struct std::formatter<wsl::windows::common::vt::Sequence, wchar_t> : std::formatter<std::wstring_view, wchar_t>
{
    auto format(const wsl::windows::common::vt::Sequence& s, std::wformat_context& ctx) const
    {
        return std::formatter<std::wstring_view, wchar_t>::format(s.Get(), ctx);
    }
};

template <>
struct std::formatter<wsl::windows::common::vt::ConstructedSequence, wchar_t> : std::formatter<wsl::windows::common::vt::Sequence, wchar_t>
{
    auto format(const wsl::windows::common::vt::ConstructedSequence& s, std::wformat_context& ctx) const
    {
        return std::formatter<wsl::windows::common::vt::Sequence, wchar_t>::format(s, ctx);
    }
};
