/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    stringshared.h

Abstract:

    This file contains shared string helper functions.

--*/

#pragma once
#include <set>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <gsl/gsl>
#include <format>
#include <source_location>

#ifndef WIN32
#include <string.h>
#include "lxdef.h"
#include "lxwil.h"
#include "defs.h"
#else
#include "string.hpp"
#endif

#define STRING_TO_WIDE_STRING_INNER(_str) L##_str
#define STRING_TO_WIDE_STRING(_str) STRING_TO_WIDE_STRING_INNER(_str)

#define GUID_FORMAT_STRING "{%08x-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x}"
#define GUID_SSCANF_STRING "%8x-%4hx-%4hx-%2hhx%2hhx-%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx"
#define GUID_BRACES_SSCANF_STRING "{" GUID_SSCANF_STRING "}"

#define MAC_ADDRESS_FORMAT_STRING "%02X%c%02X%c%02X%c%02X%c%02X%c%02X"

namespace wsl::shared::string {

using MacAddress = std::array<std::uint8_t, 6>;

inline unsigned int CopyToSpan(const std::string_view String, const gsl::span<gsl::byte> Span, size_t& Offset)
{
    gsl::copy(as_bytes(gsl::make_span(String.data(), String.size())), Span.subspan(Offset));
    const auto PreviousOffset = gsl::narrow_cast<unsigned int>(Offset);
    Offset += String.size() + 1;
    return PreviousOffset;
}

inline bool IsDriveRoot(const std::string_view Path)
{
    bool IsRoot = true;
    if (Path.length() == 3)
    {
        IsRoot &= Path[2] == '\\';
    }

    if (Path.length() == 2 || Path.length() == 3)
    {
        IsRoot &= isalpha(Path[0]) && Path[1] == ':';
    }
    else
    {
        IsRoot = false;
    }

    return IsRoot;
}

template <class T>
inline bool EndsWith(const std::basic_string<T>& String, const std::basic_string_view<T> Suffix)
{
    if (Suffix.size() > String.size())
    {
        return false;
    }

    return std::equal(Suffix.rbegin(), Suffix.rend(), String.rbegin());
}

template <class T, class TInput>
inline std::basic_string<T> Join(const std::vector<TInput>& Input, T Separator)
{
    std::basic_stringstream<T> Out;
    for (size_t Index = 0; Index < Input.size(); Index += 1)
    {
        if (Index != 0)
        {
            Out << Separator;
        }

        Out << Input[Index];
    }

    return Out.str();
}

template <class T>
inline std::vector<std::basic_string<T>> Split(const std::basic_string<T>& String, T Separator)
{
    std::vector<std::basic_string<T>> Output;
    std::basic_istringstream<T> Input(String);
    std::basic_string<T> Entry;
    while (std::getline(Input, Entry, Separator))
    {
        if (!Entry.empty())
        {
            Output.emplace_back(std::move(Entry));
        }
    }

    return Output;
}

template <class T>
inline std::vector<std::basic_string<T>> SplitByMultipleSeparators(const std::basic_string<T>& String, const std::basic_string<T>& Separators)
{
    std::vector<std::basic_string<T>> Output;
    size_t CurrentIndex = 0;

    while (true)
    {
        CurrentIndex = String.find_first_not_of(Separators, CurrentIndex);
        if (CurrentIndex == std::string::npos)
        {
            break;
        }

        const size_t NextSeparator = String.find_first_of(Separators, CurrentIndex);

        if (NextSeparator == std::string::npos)
        {
            Output.emplace_back(std::move(String.substr(CurrentIndex)));
            break;
        }
        else
        {
            Output.emplace_back(std::move(String.substr(CurrentIndex, NextSeparator - CurrentIndex)));
            CurrentIndex = NextSeparator;
        }
    }

    return Output;
}

inline const char* FromSpan(gsl::span<gsl::byte> Span, size_t Offset = 0)
{
    THROW_INVALID_ARG_IF(Span.size() < Offset);

    Span = Span.subspan(Offset);
    const std::string_view String{reinterpret_cast<const char*>(Span.data()), Span.size()};
    const auto End = String.find('\0');
    THROW_INVALID_ARG_IF(End == String.npos);

    return String.data();
}

constexpr auto c_defaultHostName = "localhost";

inline std::string CleanHostname(const std::string_view Hostname)
{
    // A valid Linux hostname:
    //  - is composed of alphanumeric characters, hyphens, and up to one dot
    //  - cannot start or end with a hyphen or a dot
    //  - cannot have a hyphen follow a dot or another hyphen
    //  - cannot be empty
    //  - cannot be longer than 64 chars
    bool dot = false;
    std::string result;
    for (const auto e : Hostname)
    {
        if (e == '.')
        {
            // There can be only one '.', it cannot be the first character, and it cannot follow a '-'.
            if (dot || result.empty() || result.back() == '-')
            {
                continue;
            }

            dot = true;
            result += e;
        }
        else if (e == '-')
        {
            // A '-' cannot be the first character, or follow another '-' or a '.'.
            if (result.empty() || result.back() == '-' || result.back() == '.')
            {
                continue;
            }

            result += e;
        }
        else if (isalnum(e))
        {
            result += e;
        }
    }

    while (!result.empty() && (result.back() == '.' || result.back() == '-'))
    {
        result.pop_back();
    }

    if (result.empty())
    {
        result = c_defaultHostName;
    }
    else if (result.size() > 64)
    {
        result.resize(64);
    }

    return result;
}

template <typename T>
inline size_t Compare(const std::basic_string_view<T> String1, const std::basic_string_view<T> String2, bool CaseInsensitive = false)
{
    // This method counts the number of matching characters at the beginning of two strings.
    std::basic_string_view<T> firstString;
    std::basic_string_view<T> secondString;
    if (String1.size() <= String2.size())
    {
        firstString = String1;
        secondString = String2;
    }
    else
    {
        firstString = String2;
        secondString = String1;
    }

    if (CaseInsensitive)
    {
        std::locale loc{"C"};
        auto result = std::mismatch(firstString.begin(), firstString.end(), secondString.begin(), [loc](T a, T b) {
            return (std::tolower(a, loc) == std::tolower(b, loc));
        });

        return (result.first - firstString.begin());
    }
    else
    {
        auto result = std::mismatch(firstString.begin(), firstString.end(), secondString.begin());
        return (result.first - firstString.begin());
    }
}

inline bool IsEqual(const std::string_view String1, const std::string_view String2, bool CaseInsensitive = false)
{
    if (String1.size() != String2.size())
    {
        return false;
    }

    return (Compare(String1, String2, CaseInsensitive) == String1.size());
}

inline bool IsEqual(const std::wstring_view String1, const std::wstring_view String2, bool CaseInsensitive = false)
{
    if (String1.size() != String2.size())
    {
        return false;
    }

    return (Compare(String1, String2, CaseInsensitive) == String1.size());
}

template <typename T>
inline std::optional<bool> ParseBool(const T* String)
{
    if (!String)
    {
        return {};
    }

    const std::basic_string_view<T> StringView(String);
    constexpr T One[] = {T('1'), T('\0')};
    constexpr T True[] = {T('t'), T('r'), T('u'), T('e'), T('\0')};
    if (IsEqual(StringView, One) || IsEqual(StringView, True, true))
    {
        return true;
    }

    constexpr T Zero[] = {T('0'), T('\0')};
    constexpr T False[] = {T('f'), T('a'), T('l'), T('s'), T('e'), T('\0')};
    if (IsEqual(StringView, Zero) || IsEqual(StringView, False, true))
    {
        return false;
    }

    return {};
}

template <typename T>
inline uint64_t ToUInt64(const T* String, T** End = nullptr, int Base = 10);

template <>
inline uint64_t ToUInt64<char>(const char* String, char** End, int Base)
{
    return std::strtoull(String, End, Base);
}

template <>
inline uint64_t ToUInt64<wchar_t>(const wchar_t* String, wchar_t** End, int Base)
{
    return std::wcstoull(String, End, Base);
}

template <typename T>
inline std::optional<uint64_t> ParseMemorySize(const T* String)
{
    if (!String)
    {
        return {};
    }

    T* End{};
    uint64_t Value = ToUInt64(String, &End, 10);
    if (Value == 0)
    {
        if (String[0] != T('0') || End != String + 1)
        {
            return {};
        }
    }

    const std::basic_string_view<T> Remainder(End);
    if (Remainder.empty())
    {
        return Value;
    }
    else if (Remainder.size() > 2)
    {
        return {};
    }

    constexpr T Bytes[] = {T('B'), T('\0')};
    constexpr T Kilobytes[] = {T('K'), T('B'), T('\0')};
    constexpr T Megabytes[] = {T('M'), T('B'), T('\0')};
    constexpr T Gigabytes[] = {T('G'), T('B'), T('\0')};
    constexpr T Terabytes[] = {T('T'), T('B'), T('\0')};
    const std::array<std::pair<std::basic_string_view<T>, uint64_t>, 5> Units{
        std::make_pair(Bytes, 1ULL),
        std::make_pair(Kilobytes, 1ULL << 10),
        std::make_pair(Megabytes, 1ULL << 20),
        std::make_pair(Gigabytes, 1ULL << 30),
        std::make_pair(Terabytes, 1ULL << 40)};

    for (const auto& [Suffix, Factor] : Units)
    {
        if ((Remainder == Suffix.substr(0, 1)) || (Remainder == Suffix))
        {
            return Value * Factor;
        }
    }

    return {};
}

inline bool StartsWith(const std::string_view String, const std::string_view Prefix, bool CaseInsensitive = false)
{
    if (String.size() < Prefix.size())
    {
        return false;
    }

    return (Compare(String.substr(0, Prefix.size()), Prefix, CaseInsensitive) == Prefix.size());
}

inline bool StartsWith(const std::wstring_view String, const std::wstring_view Prefix, bool CaseInsensitive = false)
{
    if (String.size() < Prefix.size())
    {
        return false;
    }

    return (Compare(String.substr(0, Prefix.size()), Prefix, CaseInsensitive) == Prefix.size());
}

enum GuidToStringFlags
{
    None = 0,
    AddBraces = 1,
    Uppercase = 2
};

template <typename TChar>
inline std::basic_string<TChar> GuidToString(const GUID& guid, GuidToStringFlags flags = GuidToStringFlags::AddBraces)
{
    // N.B. std::string guarantees that the null terminator is always allocated:
    //      https://en.cppreference.com/w/cpp/string/basic_string/data
    std::basic_string<TChar> output(38, '\0');

    if constexpr (std::is_same_v<TChar, char>)
    {
        snprintf(
            output.data(),
            output.size() + 1,
            GUID_FORMAT_STRING,
            static_cast<unsigned int>(guid.Data1),
            guid.Data2,
            guid.Data3,
            guid.Data4[0],
            guid.Data4[1],
            guid.Data4[2],
            guid.Data4[3],
            guid.Data4[4],
            guid.Data4[5],
            guid.Data4[6],
            guid.Data4[7]);
    }
    else if constexpr (std::is_same_v<TChar, wchar_t>)
    {
        swprintf(
            output.data(),
            output.size() + 1,
            STRING_TO_WIDE_STRING(GUID_FORMAT_STRING),
            static_cast<unsigned int>(guid.Data1),
            guid.Data2,
            guid.Data3,
            guid.Data4[0],
            guid.Data4[1],
            guid.Data4[2],
            guid.Data4[3],
            guid.Data4[4],
            guid.Data4[5],
            guid.Data4[6],
            guid.Data4[7]);
    }
    else
    {
        static_assert(sizeof(TChar) != sizeof(TChar), "Unsupported character type");
    }

    if (WI_IsFlagClear(flags, GuidToStringFlags::AddBraces))
    {
        output.erase(output.begin());
        output.pop_back();
    }

    if (WI_IsFlagSet(flags, GuidToStringFlags::Uppercase))
    {
        std::transform(output.begin(), output.end(), output.begin(), toupper);
    }

    return output;
}

template <typename TChar>
inline std::optional<GUID> ToGuid(const TChar* string, std::optional<size_t> length = {})
{
    if (!string)
    {
        return {};
    }

    if (!length.has_value())
    {
        length = std::basic_string<TChar>{string}.size();
    }

    GUID guid;
    int result{};
    if constexpr (std::is_same_v<TChar, char>)
    {
        if (length.value() == 38 && string[0] == '{' && string[37] == '}')
        {
            result = sscanf(
                string,
                GUID_BRACES_SSCANF_STRING,
                &guid.Data1,
                &guid.Data2,
                &guid.Data3,
                &guid.Data4[0],
                &guid.Data4[1],
                &guid.Data4[2],
                &guid.Data4[3],
                &guid.Data4[4],
                &guid.Data4[5],
                &guid.Data4[6],
                &guid.Data4[7]);
        }
        else if (length.value() == 36)
        {
            result = sscanf(
                string,
                GUID_SSCANF_STRING,
                &guid.Data1,
                &guid.Data2,
                &guid.Data3,
                &guid.Data4[0],
                &guid.Data4[1],
                &guid.Data4[2],
                &guid.Data4[3],
                &guid.Data4[4],
                &guid.Data4[5],
                &guid.Data4[6],
                &guid.Data4[7]);
        }
    }
    else if constexpr (std::is_same_v<TChar, wchar_t>)
    {
        if (length.value() == 38 && string[0] == '{' && string[37] == '}')
        {
            result = swscanf(
                string,
                STRING_TO_WIDE_STRING(GUID_BRACES_SSCANF_STRING),
                &guid.Data1,
                &guid.Data2,
                &guid.Data3,
                &guid.Data4[0],
                &guid.Data4[1],
                &guid.Data4[2],
                &guid.Data4[3],
                &guid.Data4[4],
                &guid.Data4[5],
                &guid.Data4[6],
                &guid.Data4[7]);
        }
        else if (length.value() == 36)
        {
            result = swscanf(
                string,
                STRING_TO_WIDE_STRING(GUID_SSCANF_STRING),
                &guid.Data1,
                &guid.Data2,
                &guid.Data3,
                &guid.Data4[0],
                &guid.Data4[1],
                &guid.Data4[2],
                &guid.Data4[3],
                &guid.Data4[4],
                &guid.Data4[5],
                &guid.Data4[6],
                &guid.Data4[7]);
        }
    }
    else
    {
        static_assert(sizeof(TChar) != sizeof(TChar), "Unsupported character type");
    }

    if (result != 11)
    {
        return {};
    }

    return guid;
}

template <typename TChar>
inline std::optional<GUID> ToGuid(const std::basic_string_view<TChar> string)
{
    return ToGuid(string.data(), string.size());
}

template <typename TChar>
inline std::optional<GUID> ToGuid(const std::basic_string<TChar>& string)
{
    return ToGuid(string.data(), string.size());
}

template <typename TChar, typename TPath>
inline std::basic_string<TChar> ReadFile(const TPath* path)
{
    std::basic_ifstream<TChar> file;
    file.exceptions(std::ios::badbit | std::ios::failbit);

    try
    {
        file.open(path);
        return std::basic_string<TChar>{std::istreambuf_iterator<TChar>(file), {}};
    }
    catch (...)
    {
        THROW_LAST_ERROR();
    }
}

inline std::wstring MultiByteToWide(const char* string)
{

#ifdef WIN32

    // This uses MultiByteToWideChar which gets the desired CP_UTF8 behavior
    return wsl::windows::common::string::MultiByteToWide(string);

#else

    if (!string)
    {
        return {};
    }

    std::mbstate_t state{};
    size_t size = std::mbsrtowcs(nullptr, &string, 0, &state);
    THROW_LAST_ERROR_IF(size == -1);

    if (size == 0)
    {
        return {};
    }

    std::wstring buffer(size, L'\0');
    std::mbsrtowcs(buffer.data(), &string, size, &state);
    return buffer;

#endif // WIN32
}

inline std::wstring MultiByteToWide(const std::string& string)
{
    return MultiByteToWide(string.c_str());
}

inline std::string WideToMultiByte(const wchar_t* string)
{

#ifdef WIN32

    // This uses WideCharToMultiByte which gets the desired CP_UTF8 behavior
    return wsl::windows::common::string::WideToMultiByte(string);

#else

    if (!string)
    {
        return {};
    }

    std::mbstate_t state{};
    size_t size = std::wcsrtombs(nullptr, &string, 0, &state);
    THROW_LAST_ERROR_IF(size == -1);

    if (size == 0)
    {
        return {};
    }

    std::string buffer(size, '\0');
    std::wcsrtombs(buffer.data(), &string, size, &state);
    return buffer;

#endif // WIN32
}

inline std::string WideToMultiByte(const std::wstring& string)
{
    return WideToMultiByte(string.c_str());
}

template <typename T>
inline uint8_t ParseNibble(T HexDigit)
{
    // Clearing bit 0x20 will turn a-f to A-F.
    return (HexDigit >= '0' && HexDigit <= '9') ? (HexDigit - '0') : ((HexDigit & ~0x20) - 'A' + 10);
}

template <typename T>
inline std::optional<MacAddress> ParseMacAddressNoThrow(const std::basic_string<T>& Input, T Separator = '\0')
{
    if (Input.size() != 17)
    {
        return {};
    }

    if (Separator == '\0')
    {
        Separator = Input[2];
        if (Separator != '-' && Separator != ':')
        {
            return {};
        }
    }

    MacAddress result;
    for (auto octet = 0; octet < 6; octet++)
    {
        size_t index = octet * 3;
        if (!std::iswxdigit(Input[index]) || !std::iswxdigit(Input[index + 1]))
        {
            return {};
        }

        if (octet < 5 && Input[index + 2] != Separator)
        {
            return {};
        }

        result[octet] = ParseNibble(Input[index]) * 16 + ParseNibble(Input[index + 1]);
    }

    return result;
}

template <typename T>
inline MacAddress ParseMacAddress(const std::basic_string<T>& Input, T Separator = '\0')
{
    auto result = ParseMacAddressNoThrow(Input, Separator);

#ifdef WIN32
    THROW_HR_IF(E_INVALIDARG, !result.has_value());
#else
    THROW_ERRNO_IF(EINVAL, !result.has_value());
#endif

    return result.value();
}

template <typename TChar>
inline std::basic_string<TChar> FormatMacAddress(const MacAddress& input, TChar separator)
{
    std::basic_string<TChar> output(17, '\0');

    if constexpr (std::is_same_v<TChar, char>)
    {
        snprintf(
            output.data(),
            output.size() + 1,
            MAC_ADDRESS_FORMAT_STRING,
            input[0],
            separator,
            input[1],
            separator,
            input[2],
            separator,
            input[3],
            separator,
            input[4],
            separator,
            input[5]);
    }
    else if constexpr (std::is_same_v<TChar, wchar_t>)
    {
        swprintf(
            output.data(),
            output.size() + 1,
            STRING_TO_WIDE_STRING(MAC_ADDRESS_FORMAT_STRING),
            input[0],
            separator,
            input[1],
            separator,
            input[2],
            separator,
            input[3],
            separator,
            input[4],
            separator,
            input[5]);
    }
    else
    {
        static_assert(sizeof(TChar) != sizeof(TChar), "Unsupported character type");
    }

    return output;
}

struct CaseInsensitiveCompare
{
    bool operator()(const std::string& left, const std::string& right) const
    {
        return _stricmp(left.c_str(), right.c_str()) < 0;
    }

    bool operator()(const char* left, const char* right) const
    {
        return _stricmp(left, right) < 0;
    }

    bool operator()(const wchar_t* left, const wchar_t* right) const
    {
        return _wcsicmp(left, right) < 0;
    }

    bool operator()(const std::wstring& left, const std::wstring& right) const
    {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

} // namespace wsl::shared::string

template <>
struct std::formatter<std::wstring, char>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const std::wstring& str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::WideToMultiByte(str));
    }
};

template <>
struct std::formatter<const wchar_t*, char>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const wchar_t* str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::WideToMultiByte(str));
    }
};

template <std::size_t N>
struct std::formatter<wchar_t[N], char>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const wchar_t str[N], TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::WideToMultiByte(str));
    }
};

template <>
struct std::formatter<std::source_location, char>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const std::source_location& location, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}[{}:{}]", location.function_name(), location.file_name(), location.line());
    }
};

template <>
struct std::formatter<char*, wchar_t>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const char* str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::MultiByteToWide(str));
    }
};

template <>
struct std::formatter<const char*, wchar_t>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const char* str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::MultiByteToWide(str));
    }
};

template <std::size_t N>
struct std::formatter<char[N], wchar_t>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const char str[N], TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::MultiByteToWide(str));
    }
};

template <class Traits, class Allocator>
struct std::formatter<std::basic_string<char, Traits, Allocator>, wchar_t>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const std::basic_string<char, Traits, Allocator>& str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::MultiByteToWide(str));
    }
};

template <>
struct std::formatter<std::filesystem::path, wchar_t>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const std::filesystem::path& str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", str.wstring());
    }
};

template <>
struct std::formatter<GUID, wchar_t>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const GUID& Guid, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::GuidToString<wchar_t>(Guid));
    }
};

template <>
struct std::formatter<wchar_t, char>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(wchar_t str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", wsl::shared::string::WideToMultiByte(std::wstring{&str, 1}));
    }
};