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
#include "ImageService.h"
#include "Localization.h"
#include <algorithm>
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

    case ArgType::Secret:
    {
        for (const auto& spec : execArgs.GetAll<ArgType::Secret>())
        {
            std::ignore = validation::ParseSecretSpec(spec);
        }
        break;
    }

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

void ValidateTimestamp(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetTimestampFromString(value, argName);
    }
}

void ValidateFormatTypeFromString(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetFormatTypeFromString(value, argName);
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

void ValidateDuration(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetDurationNanosFromString(value, argName);
    }
}

void ValidateNanoCpus(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetNanoCpusFromString(value, argName);
    }
}

void ValidateUlimit(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = ParseUlimit(value, argName);
    }
}

} // namespace wsl::windows::wslc::validation
