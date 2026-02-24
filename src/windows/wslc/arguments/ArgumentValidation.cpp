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
#include "Exceptions.h"
#include <charconv>
#include <format>
#include <unordered_map>
#include <wslaservice.h>

using namespace wsl::windows::common;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {
// Common argument validation that occurs across multiple commands.
void Argument::Validate(const ArgMap& execArgs) const
{
    switch (m_argType)
    {
    case ArgType::Signal:
        validation::ValidateWSLASignalFromString(execArgs.GetAll<ArgType::Signal>(), m_name);
        break;

    case ArgType::Time:
        validation::ValidateIntegerFromString<LONGLONG>(execArgs.GetAll<ArgType::Time>(), m_name);
        break;

    default:
        break;
    }
}
} // namespace wsl::windows::wslc

namespace wsl::windows::wslc::validation {

// Map of signal names to WSLASignal enum values
static const std::unordered_map<std::wstring, WSLASignal> SignalMap = {
    {L"SIGHUP", WSLASignalSIGHUP},   {L"SIGINT", WSLASignalSIGINT},     {L"SIGQUIT", WSLASignalSIGQUIT},
    {L"SIGILL", WSLASignalSIGILL},   {L"SIGTRAP", WSLASignalSIGTRAP},   {L"SIGABRT", WSLASignalSIGABRT},
    {L"SIGIOT", WSLASignalSIGIOT},   {L"SIGBUS", WSLASignalSIGBUS},     {L"SIGFPE", WSLASignalSIGFPE},
    {L"SIGKILL", WSLASignalSIGKILL}, {L"SIGUSR1", WSLASignalSIGUSR1},   {L"SIGSEGV", WSLASignalSIGSEGV},
    {L"SIGUSR2", WSLASignalSIGUSR2}, {L"SIGPIPE", WSLASignalSIGPIPE},   {L"SIGALRM", WSLASignalSIGALRM},
    {L"SIGTERM", WSLASignalSIGTERM}, {L"SIGTKFLT", WSLASignalSIGTKFLT}, {L"SIGCHLD", WSLASignalSIGCHLD},
    {L"SIGCONT", WSLASignalSIGCONT}, {L"SIGSTOP", WSLASignalSIGSTOP},   {L"SIGTSTP", WSLASignalSIGTSTP},
    {L"SIGTTIN", WSLASignalSIGTTIN}, {L"SIGTTOU", WSLASignalSIGTTOU},   {L"SIGURG", WSLASignalSIGURG},
    {L"SIGXCPU", WSLASignalSIGXCPU}, {L"SIGXFSZ", WSLASignalSIGXFSZ},   {L"SIGVTALRM", WSLASignalSIGVTALRM},
    {L"SIGPROF", WSLASignalSIGPROF}, {L"SIGWINCH", WSLASignalSIGWINCH}, {L"SIGIO", WSLASignalSIGIO},
    {L"SIGPOLL", WSLASignalSIGPOLL}, {L"SIGPWR", WSLASignalSIGPWR},     {L"SIGSYS", WSLASignalSIGSYS},
};

void ValidateWSLASignalFromString(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetWSLASignalFromString(value, argName);
    }
}

// Convert string to WSLASignal enum - accepts either signal name (e.g., "SIGKILL") or number (e.g., "9")
WSLASignal GetWSLASignalFromString(const std::wstring& input, const std::wstring& argName)
{
    constexpr int MIN_SIGNAL = WSLASignalSIGHUP;
    constexpr int MAX_SIGNAL = WSLASignalSIGSYS;
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
        throw ArgumentException(std::format(
            L"Invalid {} value: {} is not a recognized signal name or number (Example: SIGKILL, kill, or 9).", argName, input));
    }

    if (signalValue < MIN_SIGNAL || signalValue > MAX_SIGNAL)
    {
        throw ArgumentException(std::format(L"Invalid {} value: {} is out of valid range ({}-{}).", argName, input, MIN_SIGNAL, MAX_SIGNAL));
    }

    return static_cast<WSLASignal>(signalValue);
}
} // namespace wsl::windows::wslc::validation