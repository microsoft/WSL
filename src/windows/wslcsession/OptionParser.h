/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OptionParser.h

Abstract:

    Helper for parsing string-keyed driver options (e.g. WSLC volume driver
    options) into typed values with consistent error reporting.

    Each accessor records the keys it consumed so RejectUnknown() can flag
    options the caller passed but the driver does not support. All errors are
    surfaced through the WSLC localized message strings.

--*/

#pragma once

#include <cerrno>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>

namespace wsl::windows::service::wslc {

class OptionParser
{
public:
    NON_COPYABLE(OptionParser);

    // Options map must outlive this OptionParser.
    explicit OptionParser(const std::map<std::string, std::string>& Options) noexcept;

    // Required unsigned integer. Throws E_INVALIDARG if missing, empty,
    // signed, non-decimal, overflows, or > Max.
    template <typename T>
    T Required(std::string_view Key, T Max = (std::numeric_limits<T>::max)());

    // As Required, but returns nullopt when the key is absent.
    template <typename T>
    std::optional<T> Optional(std::string_view Key, T Max = (std::numeric_limits<T>::max)());

    // Parses an optional boolean via wsl::shared::string::ParseBool
    // ("0"/"1"/"true"/"false", case-insensitive). Throws on unknown values.
    std::optional<bool> OptionalBool(std::string_view Key);

    // Throws E_INVALIDARG on the first option that was supplied but never consumed.
    void RejectUnknown();

private:
    // Returns the value for Key (nullptr if absent) and marks it consumed.
    const std::string* Find(std::string_view Key);

    [[noreturn]] static void ThrowInvalid(std::string_view Key, const std::string& Value);
    [[noreturn]] static void ThrowMissing(std::string_view Key);
    [[noreturn]] static void ThrowUnknown(std::string_view Key);

    template <typename T>
    static T ParseUnsignedValue(std::string_view Key, const std::string& Value, T Max);

    const std::map<std::string, std::string>& m_options;
    std::set<std::string, std::less<>> m_consumed;
};

template <typename T>
inline T OptionParser::Required(std::string_view Key, T Max)
{
    const auto* value = Find(Key);
    if (value == nullptr)
    {
        ThrowMissing(Key);
    }

    return ParseUnsignedValue<T>(Key, *value, Max);
}

template <typename T>
inline std::optional<T> OptionParser::Optional(std::string_view Key, T Max)
{
    const auto* value = Find(Key);
    if (value == nullptr)
    {
        return std::nullopt;
    }

    return ParseUnsignedValue<T>(Key, *value, Max);
}

template <typename T>
inline T OptionParser::ParseUnsignedValue(std::string_view Key, const std::string& Value, T Max)
{
    static_assert(std::is_unsigned_v<T>, "OptionParser numeric accessors only support unsigned integer types");

    // strtoull treats a leading '-' as unsigned wraparound and accepts '+';
    // reject both (and empty input) up-front.
    if (Value.empty() || Value.front() == '-' || Value.front() == '+')
    {
        ThrowInvalid(Key, Value);
    }

    errno = 0;
    char* end = nullptr;
    const auto parsed = wsl::shared::string::ToUInt64(Value.c_str(), &end, 10);
    // Capture errno immediately so debug allocators/logging hooks can't stomp it.
    const int parseErrno = errno;
    if (parseErrno != 0 || end == nullptr || *end != '\0' || parsed > static_cast<uint64_t>(Max))
    {
        ThrowInvalid(Key, Value);
    }

    return static_cast<T>(parsed);
}

} // namespace wsl::windows::service::wslc
