/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.cpp

Abstract:

    Implementation of the Argument Validation.

--*/

#include "precomp.h"
#include "Argument.h"
#include "ArgumentTypes.h"
#include "ArgumentValidation.h"
#include "ContainerModel.h"
#include "Exceptions.h"
#include "Localization.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <sstream>
#include <unordered_map>
#include <wslc.h>

using namespace wsl::windows::common;
using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {
// Common argument validation that occurs across multiple commands.
void Argument::Validate(const ArgMap& execArgs) const
{
    switch (m_argType)
    {
    case ArgType::Format:
        validation::ValidateFormatTypeFromString(execArgs.GetAll<ArgType::Format>(), m_name);
        break;

    case ArgType::Signal:
        validation::ValidateWSLCSignalFromString(execArgs.GetAll<ArgType::Signal>(), m_name);
        break;

    case ArgType::StopSignal:
        validation::ValidateWSLCSignalFromString(execArgs.GetAll<ArgType::StopSignal>(), m_name);
        break;

    case ArgType::StopTimeout:
        validation::ValidateIntegerFromString<long>(execArgs.GetAll<ArgType::StopTimeout>(), m_name);
        break;

    case ArgType::ShmSize:
        validation::ValidateMemorySize(execArgs.GetAll<ArgType::ShmSize>(), m_name);
        break;

    case ArgType::HealthInterval:
        validation::ValidateDuration(execArgs.GetAll<ArgType::HealthInterval>(), m_name);
        break;

    case ArgType::HealthTimeout:
        validation::ValidateDuration(execArgs.GetAll<ArgType::HealthTimeout>(), m_name);
        break;

    case ArgType::HealthStartPeriod:
        validation::ValidateDuration(execArgs.GetAll<ArgType::HealthStartPeriod>(), m_name);
        break;

    case ArgType::HealthRetries:
        validation::ValidateIntegerFromString<int>(
            execArgs.GetAll<ArgType::HealthRetries>(), m_name, [](int value) { return value >= 0; });
        break;

    case ArgType::NoHealthcheck:
        if (execArgs.Contains(ArgType::HealthCmd) || execArgs.Contains(ArgType::HealthInterval) || execArgs.Contains(ArgType::HealthTimeout) ||
            execArgs.Contains(ArgType::HealthStartPeriod) || execArgs.Contains(ArgType::HealthRetries))
        {
            throw ArgumentException(Localization::WSLCCLI_NoHealthcheckConflictError());
        }
        break;

    case ArgType::Memory:
        validation::ValidateMemorySize(execArgs.GetAll<ArgType::Memory>(), m_name);
        break;

    case ArgType::Cpus:
        validation::ValidateNanoCpus(execArgs.GetAll<ArgType::Cpus>(), m_name);
        break;

    case ArgType::Ulimit:
        validation::ValidateUlimit(execArgs.GetAll<ArgType::Ulimit>(), m_name);
        break;

    case ArgType::Tail:
        validation::ValidateIntegerFromString<ULONGLONG>(
            execArgs.GetAll<ArgType::Tail>(), m_name, [](auto value) { return value != 0; });
        break;

    case ArgType::Time:
        validation::ValidateIntegerFromString<LONGLONG>(execArgs.GetAll<ArgType::Time>(), m_name);
        break;

    case ArgType::Since:
        validation::ValidateTimestamp(execArgs.GetAll<ArgType::Since>(), m_name);
        break;

    case ArgType::Until:
        validation::ValidateTimestamp(execArgs.GetAll<ArgType::Until>(), m_name);
        break;

    case ArgType::Last:
        validation::ValidateIntegerFromString<int>(execArgs.GetAll<ArgType::Last>(), m_name);
        break;

    case ArgType::Filter:
        validation::ValidateFilter(execArgs.GetAll<ArgType::Filter>());
        break;

    case ArgType::Gpus:
        validation::ValidateGpus(execArgs.GetAll<ArgType::Gpus>(), m_name);
        break;

    case ArgType::Volume:
        validation::ValidateVolumeMount(execArgs.GetAll<ArgType::Volume>());
        break;

    case ArgType::WorkDir:
    {
        const auto& value = execArgs.Get<ArgType::WorkDir>();
        if (value.empty() ||
            std::all_of(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)); }))
        {
            throw ArgumentException(Localization::WSLCCLI_WorkingDirEmptyError(m_name));
        }
        break;
    }

    case ArgType::Network:
    {
        for (const auto& value : execArgs.GetAll<ArgType::Network>())
        {
            if (value.empty() ||
                std::all_of(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)); }))
            {
                throw ArgumentException(Localization::WSLCCLI_NetworkEmptyError(m_name));
            }

            if (IsEqual(value, L"host", true))
            {
                throw ArgumentException(Localization::WSLCCLI_NetworkHostModeNotSupportedError());
            }
        }
        break;
    }

    case ArgType::NetworkAlias:
    {
        for (const auto& value : execArgs.GetAll<ArgType::NetworkAlias>())
        {
            if (value.empty() ||
                std::all_of(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)); }))
            {
                throw ArgumentException(Localization::WSLCCLI_NetworkAliasEmptyError(m_name));
            }
        }
        break;
    }

    default:
        break;
    }
}
} // namespace wsl::windows::wslc

namespace wsl::windows::wslc::validation {

// Map of signal names to WSLCSignal enum values
static const std::unordered_map<std::wstring, WSLCSignal> SignalMap = {
    {L"SIGHUP", WSLCSignalSIGHUP},   {L"SIGINT", WSLCSignalSIGINT},     {L"SIGQUIT", WSLCSignalSIGQUIT},
    {L"SIGILL", WSLCSignalSIGILL},   {L"SIGTRAP", WSLCSignalSIGTRAP},   {L"SIGABRT", WSLCSignalSIGABRT},
    {L"SIGIOT", WSLCSignalSIGIOT},   {L"SIGBUS", WSLCSignalSIGBUS},     {L"SIGFPE", WSLCSignalSIGFPE},
    {L"SIGKILL", WSLCSignalSIGKILL}, {L"SIGUSR1", WSLCSignalSIGUSR1},   {L"SIGSEGV", WSLCSignalSIGSEGV},
    {L"SIGUSR2", WSLCSignalSIGUSR2}, {L"SIGPIPE", WSLCSignalSIGPIPE},   {L"SIGALRM", WSLCSignalSIGALRM},
    {L"SIGTERM", WSLCSignalSIGTERM}, {L"SIGTKFLT", WSLCSignalSIGTKFLT}, {L"SIGCHLD", WSLCSignalSIGCHLD},
    {L"SIGCONT", WSLCSignalSIGCONT}, {L"SIGSTOP", WSLCSignalSIGSTOP},   {L"SIGTSTP", WSLCSignalSIGTSTP},
    {L"SIGTTIN", WSLCSignalSIGTTIN}, {L"SIGTTOU", WSLCSignalSIGTTOU},   {L"SIGURG", WSLCSignalSIGURG},
    {L"SIGXCPU", WSLCSignalSIGXCPU}, {L"SIGXFSZ", WSLCSignalSIGXFSZ},   {L"SIGVTALRM", WSLCSignalSIGVTALRM},
    {L"SIGPROF", WSLCSignalSIGPROF}, {L"SIGWINCH", WSLCSignalSIGWINCH}, {L"SIGIO", WSLCSignalSIGIO},
    {L"SIGPOLL", WSLCSignalSIGPOLL}, {L"SIGPWR", WSLCSignalSIGPWR},     {L"SIGSYS", WSLCSignalSIGSYS},
};

void ValidateWSLCSignalFromString(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetWSLCSignalFromString(value, argName);
    }
}

void ValidateVolumeMount(const std::vector<std::wstring>& values)
{
    for (const auto& value : values)
    {
        std::ignore = models::VolumeMount::Parse(value);
    }
}

// Validates that each --filter argument is in the form "key=value". Rejects entries without an '=';
// the runtime validates the key and value for specific objects.
void ValidateFilter(const std::vector<std::wstring>& values)
{
    for (const auto& value : values)
    {
        std::ignore = ParseFilter(value);
    }
}

// Convert string to WSLCSignal enum - accepts either signal name (e.g., "SIGKILL") or number (e.g., "9")
WSLCSignal GetWSLCSignalFromString(const std::wstring& input, const std::wstring& argName)
{
    constexpr int MIN_SIGNAL = WSLCSignalSIGHUP;
    constexpr int MAX_SIGNAL = WSLCSignalSIGSYS;
    constexpr std::wstring_view sigPrefix = L"SIG";

    // Normalize input: ensure it has "SIG" prefix for map lookup
    std::wstring normalizedInput;
    if (IsEqual(input.substr(0, sigPrefix.size()), sigPrefix, true))
    {
        normalizedInput = input;
    }
    else
    {
        normalizedInput = std::wstring(sigPrefix) + input;
    }

    for (const auto& [signalName, signalValue] : SignalMap)
    {
        if (IsEqual(normalizedInput, signalName, true))
        {
            return signalValue;
        }
    }

    // User may have input an integer representation instead.
    int signalValue{};
    try
    {
        signalValue = GetIntegerFromString<int>(input, argName);
    }
    // If it fails to be converted give a better user message than just the integer conversion
    // failure since we also know it failed to be found in the map.
    catch (ArgumentException)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidSignalError(argName, input));
    }

    if (signalValue < MIN_SIGNAL || signalValue > MAX_SIGNAL)
    {
        throw ArgumentException(Localization::WSLCCLI_SignalOutOfRangeError(argName, input, MIN_SIGNAL, MAX_SIGNAL));
    }

    return static_cast<WSLCSignal>(signalValue);
}

// Parses an RFC3339 timestamp (e.g. "2024-01-15T10:30:00Z" or "2024-01-15T10:30:00+05:30")
// into a ULONGLONG Unix epoch seconds value using std::chrono::parse.
// Note: +HHMM (no colon) offsets are not supported; use +HH:MM format.
static std::optional<ULONGLONG> TryParseRfc3339(const std::string& input)
{
    std::string normalized = input;

    // Normalize trailing 'Z'/'z' to '+00:00' so %Ez can parse it uniformly.
    if (!normalized.empty() && (normalized.back() == 'Z' || normalized.back() == 'z'))
    {
        normalized.pop_back();
        normalized += "+00:00";
    }

    // Reject bare dot with no fractional digits (e.g. "10:30:00.+00:00") since
    // std::chrono::parse is lenient about this.
    auto dotPos = normalized.find('.');
    if (dotPos != std::string::npos && (dotPos + 1 >= normalized.size() || !std::isdigit(normalized[dotPos + 1])))
    {
        return std::nullopt;
    }

    // Pre-validate day-of-month since std::chrono::parse silently wraps invalid dates (e.g. Feb 31 → Mar 2).
    if (normalized.size() >= 10 && normalized[4] == '-' && normalized[7] == '-')
    {
        int year = 0, month = 0, day = 0;
        auto yResult = std::from_chars(normalized.data(), normalized.data() + 4, year);
        auto mResult = std::from_chars(normalized.data() + 5, normalized.data() + 7, month);
        auto dResult = std::from_chars(normalized.data() + 8, normalized.data() + 10, day);

        if (yResult.ec == std::errc() && mResult.ec == std::errc() && dResult.ec == std::errc())
        {
            auto ymd = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                       std::chrono::day{static_cast<unsigned>(day)};
            if (!ymd.ok())
            {
                return std::nullopt;
            }
        }
    }

    // Parse into nanosecond precision so fractional seconds (e.g. ".123456789") are consumed
    // by std::chrono::parse rather than requiring manual stripping.
    std::chrono::sys_time<std::chrono::nanoseconds> utcTime;
    std::istringstream stream(normalized);
    stream >> std::chrono::parse("%FT%T%Ez", utcTime);
    if (stream.fail())
    {
        return std::nullopt;
    }

    // Reject if there are trailing characters after the parsed timestamp
    if (stream.peek() != std::istringstream::traits_type::eof())
    {
        return std::nullopt;
    }

    auto epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(utcTime.time_since_epoch()).count();
    if (epochSeconds < 0)
    {
        return std::nullopt;
    }

    return static_cast<ULONGLONG>(epochSeconds);
}

void ValidateTimestamp(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetTimestampFromString(value, argName);
    }
}

ULONGLONG GetTimestampFromString(const std::wstring& value, const std::wstring& argName)
{
    std::string narrowValue = wsl::windows::common::string::WideToMultiByte(value);

    // Try integer (Unix epoch seconds) first
    ULONGLONG intValue{};
    const char* begin = narrowValue.c_str();
    const char* end = begin + narrowValue.size();
    auto result = std::from_chars(begin, end, intValue);
    if (result.ec == std::errc() && result.ptr == end)
    {
        return intValue;
    }

    // Try RFC3339 timestamp
    auto rfc3339Value = TryParseRfc3339(narrowValue);
    if (rfc3339Value.has_value())
    {
        return rfc3339Value.value();
    }

    throw ArgumentException(Localization::WSLCCLI_InvalidTimestampArgumentError(argName, value));
}

void ValidateFormatTypeFromString(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetFormatTypeFromString(value, argName);
    }
}

FormatType GetFormatTypeFromString(const std::wstring& input, const std::wstring& argName)
{
    if (IsEqual(input, L"json"))
    {
        return FormatType::Json;
    }
    else if (IsEqual(input, L"table"))
    {
        return FormatType::Table;
    }
    else
    {
        throw ArgumentException(std::format(
            L"Invalid {} value: {} is not a recognized format type. Supported format types are: json, table.", argName, input));
    }
}

InspectType GetInspectTypeFromString(const std::wstring& input, const std::wstring& argName)
{
    if (IsEqual(input, L"image"))
    {
        return InspectType::Image;
    }
    else if (IsEqual(input, L"container"))
    {
        return InspectType::Container;
    }
    else if (IsEqual(input, L"network"))
    {
        return InspectType::Network;
    }
    else if (IsEqual(input, L"volume"))
    {
        return InspectType::Volume;
    }
    else
    {
        constexpr std::wstring_view supportedValues = L"image, container, network, volume";
        throw ArgumentException(Localization::WSLCCLI_InvalidInspectError(argName, input, supportedValues));
    }
}

void ValidateGpus(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        if (!IsEqual(value, L"all"))
        {
            throw ArgumentException(Localization::WSLCCLI_GpusInvalidValue(argName, value));
        }
    }
}

void ValidateMemorySize(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetMemorySizeFromString(value, argName);
    }
}

int64_t GetMemorySizeFromString(const std::wstring& input, const std::wstring& argName)
{
    auto parsed = wsl::shared::string::ParseMemorySize(input.c_str());
    if (!parsed.has_value())
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidMemorySizeError(argName, input));
    }

    return static_cast<int64_t>(parsed.value());
}

// Parses duration string into nanoseconds.
static std::optional<int64_t> TryParseDuration(const std::string& input)
{
    if (input.empty())
    {
        return std::nullopt;
    }

    size_t pos = 0;
    bool negative = false;
    if (input[pos] == '+' || input[pos] == '-')
    {
        negative = input[pos] == '-';
        pos++;
    }

    // Special case: a bare "0" (with optional sign) is a valid zero duration.
    if (input.substr(pos) == "0")
    {
        return 0;
    }

    // Accumulate in a long double so fractional units (e.g. "1.5h") are handled, then round.
    long double totalNanos = 0.0L;
    bool sawValue = false;

    while (pos < input.size())
    {
        // Parse the numeric part (integer and/or fraction).
        const size_t numberStart = pos;
        while (pos < input.size() && (std::isdigit(static_cast<unsigned char>(input[pos])) || input[pos] == '.'))
        {
            pos++;
        }

        const std::string numberStr = input.substr(numberStart, pos - numberStart);
        if (numberStr.empty() || numberStr == "." || std::count(numberStr.begin(), numberStr.end(), '.') > 1)
        {
            return std::nullopt;
        }

        // Parse the unit (everything up to the next digit or '.').
        const size_t unitStart = pos;
        while (pos < input.size() && !std::isdigit(static_cast<unsigned char>(input[pos])) && input[pos] != '.')
        {
            pos++;
        }

        const std::string unit = input.substr(unitStart, pos - unitStart);

        long double multiplier{};
        if (unit == "ns")
        {
            multiplier = 1.0L;
        }
        else if (unit == "us" || unit == "\xC2\xB5s" /* µs (U+00B5) */ || unit == "\xCE\xBCs" /* μs (U+03BC) */)
        {
            multiplier = 1000L;
        }
        else if (unit == "ms")
        {
            multiplier = 1000000L;
        }
        else if (unit == "s")
        {
            multiplier = 1000000000L;
        }
        else if (unit == "m")
        {
            multiplier = 60000000000L;
        }
        else if (unit == "h")
        {
            multiplier = 3600000000000L;
        }
        else
        {
            return std::nullopt;
        }

        long double value{};
        try
        {
            auto [ptr, ec] = std::from_chars(numberStr.data(), numberStr.data() + numberStr.size(), value, std::chars_format::fixed);
            if (ptr != numberStr.data() + numberStr.size() || ec != std::errc())
            {
                return std::nullopt;
            }
        }
        catch (...)
        {
            return std::nullopt;
        }

        totalNanos += value * multiplier;
        sawValue = true;
    }

    if (!sawValue)
    {
        return std::nullopt;
    }

    if (negative)
    {
        totalNanos = -totalNanos;
    }

    if (totalNanos > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        totalNanos < static_cast<long double>(std::numeric_limits<int64_t>::min()))
    {
        return std::nullopt;
    }

    return static_cast<int64_t>(std::llroundl(totalNanos));
}

void ValidateDuration(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetDurationNanosFromString(value, argName);
    }
}

int64_t GetDurationNanosFromString(const std::wstring& input, const std::wstring& argName)
{
    const std::string narrow = WideToMultiByte(input);
    const auto parsed = TryParseDuration(narrow);

    if (!parsed.has_value() || parsed.value() < 0)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidDurationError(argName, input));
    }

    return parsed.value();
}

void ValidateNanoCpus(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetNanoCpusFromString(value, argName);
    }
}

int64_t GetNanoCpusFromString(const std::wstring& input, const std::wstring& argName)
{
    constexpr double NanosPerCpu = 1'000'000'000.0;
    constexpr double MaxCpus = static_cast<double>(std::numeric_limits<int64_t>::max()) / NanosPerCpu;

    const std::string narrow = WideToMultiByte(input);
    const char* begin = narrow.c_str();
    const char* end = begin + narrow.size();

    double cpus{};
    const auto result = std::from_chars(begin, end, cpus, std::chars_format::fixed);
    if (result.ec != std::errc() || result.ptr != end || cpus <= 0.0 || cpus > MaxCpus)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidCpusError(argName, input));
    }

    return static_cast<int64_t>(cpus * NanosPerCpu);
}

void ValidateUlimit(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = ParseUlimit(value, argName);
    }
}

std::tuple<std::string, int64_t, int64_t> ParseUlimit(const std::wstring& input, const std::wstring& argName)
{
    // Accepts <name>=<soft>[:<hard>]; if hard is omitted hard = soft. -1 means unlimited.
    const auto equalsPos = input.find(L'=');
    if (equalsPos == std::wstring::npos || equalsPos == 0)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
    }

    const std::wstring valuesPart = input.substr(equalsPos + 1);
    const auto colonPos = valuesPart.find(L':');

    auto parseLimit = [&](const std::wstring& limitStr) -> int64_t {
        if (limitStr.empty())
        {
            throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
        }

        try
        {
            return GetIntegerFromString<int64_t>(limitStr, argName, [](int64_t v) { return v >= -1; });
        }
        catch (const ArgumentException&)
        {
            // Re-throw with the ulimit-specific error message so the user sees the full input.
            throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
        }
    };

    const int64_t soft = parseLimit(colonPos == std::wstring::npos ? valuesPart : valuesPart.substr(0, colonPos));
    const int64_t hard = colonPos == std::wstring::npos ? soft : parseLimit(valuesPart.substr(colonPos + 1));

    // This rejects "-1:1024" and "-1:<finite>" while allowing "<finite>:-1", "-1:-1", and "-1".
    const bool invalidRange = (soft == -1) ? (hard != -1) : (hard != -1 && hard < soft);
    if (invalidRange)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidUlimitError(argName, input));
    }

    return {WideToMultiByte(input.substr(0, equalsPos)), soft, hard};
}

std::pair<std::string, std::string> ParseLabel(const std::wstring& value)
{
    std::pair<std::string, std::string> result{};
    auto pos = value.find('=');
    if (pos == std::wstring::npos)
    {
        result.first = WideToMultiByte(value);
    }
    else
    {
        result.first = WideToMultiByte(value.substr(0, pos));
        result.second = WideToMultiByte(value.substr(pos + 1));
    }

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::WSLCCLI_LabelKeyEmptyError(), result.first.empty());
    return result;
}

std::pair<std::string, std::string> ParseDriverOption(const std::wstring& value)
{
    auto pos = value.find('=');
    if (pos == std::wstring::npos)
    {
        return {WideToMultiByte(value), std::string{}};
    }

    return {WideToMultiByte(value.substr(0, pos)), WideToMultiByte(value.substr(pos + 1))};
}

std::pair<std::string, std::string> ParseFilter(const std::wstring& value)
{
    auto pos = value.find(L'=');
    if (pos == std::wstring::npos)
    {
        throw ArgumentException(Localization::WSLCCLI_InvalidFilterError(value));
    }

    return {WideToMultiByte(value.substr(0, pos)), WideToMultiByte(value.substr(pos + 1))};
}

} // namespace wsl::windows::wslc::validation
