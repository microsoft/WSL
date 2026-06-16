/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.cpp

Abstract:

    Implementation of the Argument Validation.

--*/
#include "Argument.h"
#include "ArgumentTypes.h"
#include "ArgumentValidation.h"
#include "ContainerModel.h"
#include "Exceptions.h"
#include "Localization.h"
#include <charconv>
#include <format>
#include <optional>
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

    case ArgType::ShmSize:
        validation::ValidateMemorySize(execArgs.GetAll<ArgType::ShmSize>(), m_name);
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
            throw ArgumentException(std::format(L"Invalid {} argument value: working directory cannot be empty or whitespace", m_name));
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
        if (value.find(L'=') == std::wstring::npos)
        {
            throw ArgumentException(Localization::WSLCCLI_InvalidFilterError(value));
        }
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
// or a plain Unix epoch integer (seconds) into a ULONGLONG epoch value.
static std::optional<ULONGLONG> TryParseRfc3339(const std::string& input)
{
    // Minimum RFC3339: "YYYY-MM-DDTHH:MM:SSZ" = 20 chars
    if (input.size() < 20)
    {
        return std::nullopt;
    }

    // Basic structural checks
    if (input[4] != '-' || input[7] != '-' || (input[10] != 'T' && input[10] != 't') || input[13] != ':' || input[16] != ':')
    {
        return std::nullopt;
    }

    auto parseDigits = [&](size_t offset, size_t count) -> std::optional<int> {
        int value = 0;
        for (size_t i = 0; i < count; ++i)
        {
            char c = input[offset + i];
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    };

    auto year = parseDigits(0, 4);
    auto month = parseDigits(5, 2);
    auto day = parseDigits(8, 2);
    auto hour = parseDigits(11, 2);
    auto minute = parseDigits(14, 2);
    auto second = parseDigits(17, 2);

    if (!year || !month || !day || !hour || !minute || !second)
    {
        return std::nullopt;
    }

    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60)
    {
        return std::nullopt;
    }

    // Parse timezone offset (after seconds and optional fractional seconds)
    size_t pos = 19;

    // Skip fractional seconds (e.g. ".123456789")
    if (pos < input.size() && input[pos] == '.')
    {
        ++pos;
        while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
        {
            ++pos;
        }
    }

    int offsetSeconds = 0;
    if (pos >= input.size())
    {
        return std::nullopt; // No timezone info
    }

    char tzChar = input[pos];
    size_t expectedEnd = pos + 1;
    if (tzChar == 'Z' || tzChar == 'z')
    {
        offsetSeconds = 0;
    }
    else if (tzChar == '+' || tzChar == '-')
    {
        if (pos + 5 > input.size())
        {
            return std::nullopt;
        }

        // Accept both HH:MM and HHMM
        size_t tzHourOffset = pos + 1;
        size_t tzMinOffset;
        if (pos + 6 <= input.size() && input[pos + 3] == ':')
        {
            tzMinOffset = pos + 4;
            expectedEnd = pos + 6;
        }
        else
        {
            tzMinOffset = pos + 3;
            expectedEnd = pos + 5;
        }

        auto tzHour = parseDigits(tzHourOffset, 2);
        auto tzMin = parseDigits(tzMinOffset, 2);
        if (!tzHour || !tzMin || *tzHour > 23 || *tzMin > 59)
        {
            return std::nullopt;
        }

        offsetSeconds = (*tzHour * 3600 + *tzMin * 60);
        if (tzChar == '-')
        {
            offsetSeconds = -offsetSeconds;
        }
    }
    else
    {
        return std::nullopt;
    }

    // Reject trailing characters after the timezone designator
    if (expectedEnd != input.size())
    {
        return std::nullopt;
    }

    // Reject pre-epoch dates
    if (*year < 1970)
    {
        return std::nullopt;
    }

    // Convert to Unix epoch using a simplified calculation
    // Days from year 1970 to the given year
    auto isLeapYear = [](int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); };

    constexpr int daysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    long long days = 0;
    for (int y = 1970; y < *year; ++y)
    {
        days += isLeapYear(y) ? 366 : 365;
    }
    for (int m = 1; m < *month; ++m)
    {
        days += daysInMonth[m];
        if (m == 2 && isLeapYear(*year))
        {
            days += 1;
        }
    }
    days += *day - 1;

    long long epochSeconds = days * 86400LL + *hour * 3600LL + *minute * 60LL + *second;
    epochSeconds -= offsetSeconds;

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
    else if (IsEqual(input, L"volume"))
    {
        return InspectType::Volume;
    }
    else
    {
        constexpr std::wstring_view supportedValues = L"image, container, volume";
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

} // namespace wsl::windows::wslc::validation
