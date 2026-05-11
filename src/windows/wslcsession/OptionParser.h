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

    // The supplied options map must outlive this OptionParser. Required/Optional/
    // OptionalBool return parsed values by value; only Find() (used internally)
    // returns a pointer into the map.
    explicit OptionParser(const std::map<std::string, std::string>& Options) noexcept;

    // Parses a required unsigned integer value. Throws E_INVALIDARG with
    // MessageWslcMissingVolumeOption if Key is absent, or
    // MessageWslcInvalidVolumeOption if the value is empty, has a leading
    // sign, contains non-digit characters, overflows, or exceeds Max.
    template <typename T>
    T Required(std::string_view Key, T Max = (std::numeric_limits<T>::max)());

    // Same validation as Required, but returns nullopt when the key is absent.
    template <typename T>
    std::optional<T> Optional(std::string_view Key, T Max = (std::numeric_limits<T>::max)());

    // Parses an optional boolean using wsl::shared::string::ParseBool semantics
    // (accepts "0"/"1"/"true"/"false", case-insensitive). Throws on unknown
    // values; returns nullopt when the key is absent.
    std::optional<bool> OptionalBool(std::string_view Key);

    // Throws E_INVALIDARG with MessageWslcInvalidVolumeOption on the first
    // option that was supplied but never consumed by Required/Optional/etc.
    void RejectUnknown();

private:
    // Returns a pointer to the value for Key (or nullptr if absent) and marks
    // the key as consumed.
    const std::string* Find(std::string_view Key);

    [[noreturn]] static void ThrowInvalid(std::string_view Key, const std::string& Value);
    [[noreturn]] static void ThrowMissing(std::string_view Key);

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

    // ToUInt64 (which wraps strtoull) interprets a leading '-' as wraparound
    // (e.g. "-1" parses to ULLONG_MAX) and silently accepts a leading '+'.
    // Reject either explicit sign and empty input up-front so signed/empty
    // options are reported as user errors instead of producing surprising
    // unsigned values.
    if (Value.empty() || Value.front() == '-' || Value.front() == '+')
    {
        ThrowInvalid(Key, Value);
    }

    errno = 0;
    char* end = nullptr;
    const auto parsed = wsl::shared::string::ToUInt64(Value.c_str(), &end, 10);
    // Capture errno immediately so any subsequent call (debug allocators,
    // logging hooks, etc.) cannot stomp on it before we inspect it.
    const int parseErrno = errno;
    if (parseErrno != 0 || end == nullptr || *end != '\0' || parsed > static_cast<uint64_t>(Max))
    {
        ThrowInvalid(Key, Value);
    }

    return static_cast<T>(parsed);
}

} // namespace wsl::windows::service::wslc
