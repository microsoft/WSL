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

    case ArgType::Time:
        validation::ValidateIntegerFromString<LONGLONG>(execArgs.GetAll<ArgType::Time>(), m_name);
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
    
    if (IsEqual(input, L"table"))
    {
        return FormatType::Table;
    }

    return FormatType::Template;
}

} // namespace wsl::windows::wslc::validation