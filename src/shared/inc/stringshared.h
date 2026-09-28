/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    stringshared.h

Abstract:

    This file contains shared string helper functions.

--*/

#pragma once
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <set>
#include <vector>
#include <string>
#include <string_view>
#include <sstream>
#include <fstream>
#include <optional>
#include <gsl/gsl>
#include <format>
#include <source_location>
#include <type_traits>

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
    Span[Offset + String.size()] = gsl::byte{0};
    const auto PreviousOffset = gsl::narrow_cast<unsigned int>(Offset);
    Offset += String.size() + 1;
    return PreviousOffset;
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

// Lowercases ASCII 'A'-'Z' only, leaving every other code unit untouched. Unlike std::tolower this is
// locale-independent (no Turkish-'I' surprises) and has no signed-char UB, which is what you want when
// normalizing ASCII protocol tokens such as buildx CSV keys.
template <class T>
inline std::basic_string<T> AsciiToLower(const std::basic_string_view<T>& String)
{
    std::basic_string<T> Result(String);
    for (auto& Ch : Result)
    {
        if (Ch >= static_cast<T>('A') && Ch <= static_cast<T>('Z'))
        {
            Ch = static_cast<T>(Ch - static_cast<T>('A') + static_cast<T>('a'));
        }
    }

    return Result;
}

// Trims leading and trailing ASCII whitespace (space, tab, CR, LF, vertical tab, form feed), matching
// the ASCII subset of Go's strings.TrimSpace. Returns a view into the input, so the input must outlive
// the result. The stdlib has no trim, so this centralizes the find_first/last_not_of idiom.
template <class T>
inline std::basic_string_view<T> TrimAscii(const std::basic_string_view<T>& String)
{
    constexpr T Whitespace[] = {
        static_cast<T>(' '), static_cast<T>('\t'), static_cast<T>('\r'), static_cast<T>('\n'), static_cast<T>('\v'), static_cast<T>('\f'), static_cast<T>('\0')};

    const auto First = String.find_first_not_of(Whitespace);
    if (First == std::basic_string_view<T>::npos)
    {
        return {};
    }

    const auto Last = String.find_last_not_of(Whitespace);
    return String.substr(First, Last - First + 1);
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
inline std::vector<std::basic_string_view<T>> SplitPreserveEmpty(const std::basic_string_view<T> String, T Separator)
{
    std::vector<std::basic_string_view<T>> Output;
    size_t Start = 0;
    while (Start <= String.size())
    {
        const auto End = String.find(Separator, Start);
        if (End == std::basic_string_view<T>::npos)
        {
            Output.emplace_back(String.substr(Start));
            break;
        }

        Output.emplace_back(String.substr(Start, End - Start));
        Start = End + 1;
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

// Splits a single CSV record into fields using the grammar Go's encoding/csv applies to one record
// (which docker buildx relies on via go-csvvalue), so a spec is parsed the way buildx would:
//   - fields are separated by commas;
//   - a field may be wrapped in double quotes, in which case a comma is a literal character and a
//     doubled quote ("") is a single literal quote;
//   - an unquoted field may not contain a double quote (Go's non-lazy ErrBareQuote).
// Returns std::nullopt when the record is malformed: an unterminated quoted field, text immediately
// after a closing quote, or a bare quote in an unquoted field.
//
// Deviation from Go/RFC 4180: this parses exactly one record. Go would treat an unquoted CR/LF as a
// record separator; here CR/LF are always ordinary field characters (never a record separator), which
// is what a single-line command-line spec needs.
template <class T>
inline std::optional<std::vector<std::basic_string<T>>> SplitCsvFields(const std::basic_string<T>& Record)
{
    constexpr T Quote = static_cast<T>('"');
    constexpr T Comma = static_cast<T>(',');

    std::vector<std::basic_string<T>> Fields;
    std::basic_string<T> Field;
    const size_t Length = Record.size();
    size_t Index = 0;

    while (true)
    {
        Field.clear();
        if (Index < Length && Record[Index] == Quote)
        {
            ++Index;
            bool Closed = false;
            while (Index < Length)
            {
                if (Record[Index] == Quote)
                {
                    // A doubled quote inside a quoted field is a single literal quote.
                    if (Index + 1 < Length && Record[Index + 1] == Quote)
                    {
                        Field.push_back(Quote);
                        Index += 2;
                        continue;
                    }

                    ++Index;
                    Closed = true;
                    break;
                }

                Field.push_back(Record[Index]);
                ++Index;
            }

            if (!Closed)
            {
                return std::nullopt; // unterminated quoted field
            }

            // After a closing quote only a comma (end of field) or end of record is valid.
            if (Index < Length && Record[Index] != Comma)
            {
                return std::nullopt;
            }
        }
        else
        {
            while (Index < Length && Record[Index] != Comma)
            {
                // Go (non-lazy) rejects a bare double quote in an unquoted field (ErrBareQuote).
                if (Record[Index] == Quote)
                {
                    return std::nullopt;
                }

                Field.push_back(Record[Index]);
                ++Index;
            }
        }

        Fields.push_back(Field);
        if (Index >= Length)
        {
            break;
        }

        ++Index; // consume the ',' and start the next field
    }

    return Fields;
}

// CSV-escapes a single field for round-tripping through JoinCsvFields/SplitCsvFields: if the field
// contains a comma, a double quote, CR, LF, or a leading/trailing space it is wrapped in double quotes
// with each embedded quote doubled; otherwise it is returned unchanged. This is not a byte-for-byte
// match of Go's encoding/csv writer (which quotes only a leading space and leaves an empty field
// unquoted); it is the minimal quoting needed for every field to parse back via SplitCsvFields.
template <class T>
inline std::basic_string<T> CsvEscapeField(const std::basic_string<T>& Field)
{
    constexpr T Quote = static_cast<T>('"');
    const T Special[] = {static_cast<T>(','), Quote, static_cast<T>('\r'), static_cast<T>('\n'), static_cast<T>(0)};

    const bool NeedsQuote = Field.find_first_of(Special) != std::basic_string<T>::npos ||
                            (!Field.empty() && (Field.front() == static_cast<T>(' ') || Field.back() == static_cast<T>(' ')));
    if (!NeedsQuote)
    {
        return Field;
    }

    std::basic_string<T> Result;
    Result.reserve(Field.size() + 2);
    Result.push_back(Quote);
    for (const T Ch : Field)
    {
        if (Ch == Quote)
        {
            Result.push_back(Quote);
        }

        Result.push_back(Ch);
    }

    Result.push_back(Quote);
    return Result;
}

// Joins fields into a single CSV record, escaping each field as needed (see CsvEscapeField). The
// result parses back to the original fields via SplitCsvFields.
template <class T>
inline std::basic_string<T> JoinCsvFields(const std::vector<std::basic_string<T>>& Fields)
{
    std::basic_string<T> Record;
    for (size_t Index = 0; Index < Fields.size(); ++Index)
    {
        if (Index != 0)
        {
            Record.push_back(static_cast<T>(','));
        }

        Record += CsvEscapeField(Fields[Index]);
    }

    return Record;
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

template <typename T>
inline const char* FromMessageBuffer(const gsl::span<gsl::byte>& Span)
{
    return FromSpan(Span, offsetof(T, Buffer));
}

inline std::vector<const char*> StringPointersFromArray(const std::vector<std::string>& Strings, bool insertNull)
{
    std::vector<const char*> result(Strings.size());
    std::transform(Strings.begin(), Strings.end(), result.begin(), [](const std::string& str) { return str.c_str(); });

    if (insertNull)
    {
        result.push_back(nullptr);
    }

    return result;
}

inline std::vector<std::string> ArrayFromSpan(gsl::span<const gsl::byte> Span, size_t Offset = 0)
{
    THROW_INVALID_ARG_IF(Span.size() < Offset);

    Span = Span.subspan(Offset);

    std::vector<std::string> Result;

    auto it = Span.begin();

    auto readSize = [&]() {
        THROW_INVALID_ARG_IF(Span.end() - it < sizeof(int32_t));

        auto size = *reinterpret_cast<const int32_t*>(&*it);
        it += sizeof(int32_t);

        return size;
    };

    while (true)
    {
        auto size = readSize();
        if (size == -1)
        {
            break;
        }

        THROW_INVALID_ARG_IF(size < 0);
        THROW_INVALID_ARG_IF(size > Span.end() - it);

        const char* begin = reinterpret_cast<const char*>(&*it);
        Result.emplace_back(begin, size);

        it += size;
    }

    return Result;
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

    if (result.size() > 64)
    {
        result.resize(64);
    }

    while (!result.empty() && (result.back() == '.' || result.back() == '-'))
    {
        result.pop_back();
    }

    if (result.empty())
    {
        result = c_defaultHostName;
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

template <class T>
inline bool IsEmptyOrWhitespace(const std::basic_string_view<T> String)
{
    return String.empty() || std::all_of(String.begin(), String.end(), [](T Ch) {
               if constexpr (std::is_same_v<T, wchar_t>)
               {
                   return std::iswspace(static_cast<wint_t>(Ch));
               }
               else
               {
                   return std::isspace(static_cast<unsigned char>(Ch));
               }
           });
}

// Parses a boolean from a string. By default only "1"/"0" and "true"/"false"
// (case-insensitive) are recognized. When AllowExtendedForms is true the single
// character forms "t"/"f" (case-insensitive) are also accepted, matching the full
// set understood by Go's strconv.ParseBool (and therefore the Docker CLI).
template <typename T>
inline std::optional<bool> ParseBool(const T* String, bool AllowExtendedForms = false)
{
    if (!String)
    {
        return {};
    }

    const std::basic_string_view<T> StringView(String);
    constexpr T One[] = {T('1'), T('\0')};
    constexpr T True[] = {T('t'), T('r'), T('u'), T('e'), T('\0')};
    constexpr T ShortTrue[] = {T('t'), T('\0')};
    if (IsEqual(StringView, One) || IsEqual(StringView, True, true) || (AllowExtendedForms && IsEqual(StringView, ShortTrue, true)))
    {
        return true;
    }

    constexpr T Zero[] = {T('0'), T('\0')};
    constexpr T False[] = {T('f'), T('a'), T('l'), T('s'), T('e'), T('\0')};
    constexpr T ShortFalse[] = {T('f'), T('\0')};
    if (IsEqual(StringView, Zero) || IsEqual(StringView, False, true) || (AllowExtendedForms && IsEqual(StringView, ShortFalse, true)))
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

template <typename TChar>
inline std::basic_string<TChar> Trim(const std::basic_string<TChar>& input)
{
    constexpr TChar whitespace[] = {TChar(' '), TChar('\t'), TChar('\n'), TChar('\r'), TChar('\f'), TChar('\v'), TChar('\0')};
    const auto first = input.find_first_not_of(whitespace);
    if (first == std::basic_string<TChar>::npos)
    {
        return {};
    }

    const auto last = input.find_last_not_of(whitespace);
    return input.substr(first, last - first + 1);
}

template <typename TChar>
inline std::basic_string<TChar> UnescapeShell(const std::basic_string<TChar>& input)
{
    enum class Quote
    {
        None,
        Single,
        Double
    };

    Quote quote = Quote::None;
    std::basic_string<TChar> output;
    output.reserve(input.size());

    for (size_t index = 0; index < input.size(); index += 1)
    {
        const auto current = input[index];
        if (quote == Quote::Single)
        {
            if (current == TChar('\''))
            {
                quote = Quote::None;
            }
            else
            {
                output.push_back(current);
            }

            continue;
        }

        if (current == TChar('\''))
        {
            if (quote == Quote::Double)
            {
                output.push_back(current);
            }
            else
            {
                quote = Quote::Single;
            }

            continue;
        }

        if (current == TChar('"'))
        {
            quote = (quote == Quote::Double) ? Quote::None : Quote::Double;
            continue;
        }

        if (current != TChar('\\'))
        {
            output.push_back(current);
            continue;
        }

        if (++index == input.size())
        {
            // return the original string if the escape is invalid.
            return input;
        }

        const auto escaped = input[index];
        if (quote == Quote::None)
        {
            // "\\\n" out of escape means continue the line.
            if (escaped != TChar('\n'))
            {
                output.push_back(escaped);
            }
        }
        else if (escaped == TChar('"') || escaped == TChar('\\') || escaped == TChar('$') || escaped == TChar('`'))
        {
            output.push_back(escaped);
        }
        else if (escaped != TChar('\n'))
        {
            output.push_back(current);
            output.push_back(escaped);
        }
    }

    // return the original string if the escape is invalid.
    return (quote == Quote::None) ? output : input;
}

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
struct std::formatter<std::source_location, wchar_t>
{
    template <typename TCtx>
    static constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(const std::source_location& location, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), L"{}[{}:{}]", location.function_name(), location.file_name(), location.line());
    }
};

// char -> wchar_t formatting is only used by the Windows components. libc++ (used to
// build the Linux components) now provides these as deleted specializations per C++23
// [format.formatter.spec], which would collide, so restrict them to Windows.
#ifdef WIN32

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
        return std::format_to(ctx.out(), L"{}", wsl::shared::string::MultiByteToWide(str));
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
        return std::format_to(ctx.out(), L"{}", wsl::shared::string::MultiByteToWide(str));
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
        return std::format_to(ctx.out(), L"{}", wsl::shared::string::MultiByteToWide(str));
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
        return std::format_to(ctx.out(), L"{}", wsl::shared::string::MultiByteToWide(str));
    }
};

#endif // WIN32

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