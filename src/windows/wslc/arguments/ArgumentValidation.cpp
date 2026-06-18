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
#include <charconv>
#include <format>
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
        validation::ValidateIntegerFromString<ULONGLONG>(execArgs.GetAll<ArgType::Since>(), m_name);
        break;

    case ArgType::Until:
        validation::ValidateIntegerFromString<ULONGLONG>(execArgs.GetAll<ArgType::Until>(), m_name);
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
