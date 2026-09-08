/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.cpp

Abstract:

    Implementation of the Argument Validation.

--*/

#include "precomp.h"
#include "Argument.h"
#include "ArgMap.h"
#include "ArgumentValidation.h"
#include "ContainerModel.h"
#include "Exceptions.h"
#include "ImageService.h"
#include "Localization.h"
#include "MountSpecParsing.h"
#include <algorithm>
#include <type_traits>
#include <utility>
#include <wslc.h>

using namespace wsl::windows::common;
using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {

namespace mount = wsl::windows::common::mount;

namespace argument::details {
    struct RawArgMapAccess
    {
        template <ArgType E>
        static auto GetAll(const ArgMap& map)
        {
            return map.GetAll<E>();
        }
    };
} // namespace argument::details

namespace {
    using argument::details::RawArgMapAccess;

    // Converts each raw value for argument A using the provided converter and caches the result on
    // the ArgMap. This is the single point where an argument's string input is converted; execution
    // later reads the cached value via GetValue/GetAllValues.
    template <ArgType A, typename Converter>
    void CacheConverted(ArgMap& execArgs, const std::wstring& argName, Converter&& convert)
    {
        using value_t = typename wsl::windows::wslc::argument::details::ArgConvertedTypeMapping<A>::value_t;
        using converted_t = decltype(convert(std::declval<const std::wstring&>(), std::declval<const std::wstring&>()));
        static_assert(
            std::is_same_v<converted_t, value_t>,
            "converter return type must exactly match the argument's declared ConvertedType in ArgumentDefinitions.h");

        for (const auto& value : RawArgMapAccess::GetAll<A>(execArgs))
        {
            execArgs.AddValidated<A>(convert(value, argName));
        }

        // Sanity check: each raw value for this argument must produce exactly one cached value.
        WI_ASSERT(execArgs.CountValidated(A) == execArgs.Count(A));
    }
} // namespace

// Common per-argument validation, run both by the up-front pass and on demand from ArgMap's read
// path. Arguments with a converted type are converted and cached here; the type is recorded as
// validated on success.
void Argument::Validate(ArgMap& execArgs) const
{
    if (execArgs.IsValidated(m_argType))
    {
        return;
    }

    switch (m_argType)
    {
    case ArgType::BuildLabel:
        for (const auto& value : RawArgMapAccess::GetAll<ArgType::BuildLabel>(execArgs))
        {
            validation::ParseLabel(value);
        }
        break;

    case ArgType::BuildOutput:
        CacheConverted<ArgType::BuildOutput>(
            execArgs, m_name, [](const std::wstring& value, const std::wstring&) { return validation::ParseOutputSpec(value); });
        break;

    case ArgType::Format:
        CacheConverted<ArgType::Format>(execArgs, m_name, validation::GetFormatTypeFromString);
        break;

    case ArgType::InspectFormat:
        CacheConverted<ArgType::InspectFormat>(execArgs, m_name, validation::GetInspectJsonIndentFromString);
        break;

    case ArgType::Pull:
        CacheConverted<ArgType::Pull>(execArgs, m_name, validation::GetPullPolicyFromString);
        break;

    case ArgType::Progress:
        CacheConverted<ArgType::Progress>(execArgs, m_name, validation::GetProgressModeFromString);
        break;

    case ArgType::Signal:
        CacheConverted<ArgType::Signal>(execArgs, m_name, validation::GetWSLCSignalFromString);
        break;

    case ArgType::StopSignal:
        CacheConverted<ArgType::StopSignal>(execArgs, m_name, validation::GetWSLCSignalFromString);
        break;

    case ArgType::StopTimeout:
        CacheConverted<ArgType::StopTimeout>(execArgs, m_name, [](const std::wstring& value, const std::wstring& name) {
            return validation::GetIntegerFromString<int>(value, name);
        });
        break;

    case ArgType::ShmSize:
        CacheConverted<ArgType::ShmSize>(execArgs, m_name, validation::GetMemorySizeFromString);
        break;

    case ArgType::HealthInterval:
        CacheConverted<ArgType::HealthInterval>(execArgs, m_name, validation::GetDurationNanosFromString);
        break;

    case ArgType::HealthTimeout:
        CacheConverted<ArgType::HealthTimeout>(execArgs, m_name, validation::GetDurationNanosFromString);
        break;

    case ArgType::HealthStartPeriod:
        CacheConverted<ArgType::HealthStartPeriod>(execArgs, m_name, validation::GetDurationNanosFromString);
        break;

    case ArgType::HealthRetries:
        CacheConverted<ArgType::HealthRetries>(execArgs, m_name, [](const std::wstring& value, const std::wstring& name) {
            return validation::GetIntegerFromString<int>(value, name, [](int v) { return v >= 0; });
        });
        break;

    case ArgType::NoHealthcheck:
        if (execArgs.Contains(ArgType::HealthCmd) || execArgs.Contains(ArgType::HealthInterval) || execArgs.Contains(ArgType::HealthTimeout) ||
            execArgs.Contains(ArgType::HealthStartPeriod) || execArgs.Contains(ArgType::HealthRetries))
        {
            std::vector<Argument> conflictingArguments{*this};
            for (const auto type :
                 {ArgType::HealthCmd, ArgType::HealthInterval, ArgType::HealthTimeout, ArgType::HealthStartPeriod, ArgType::HealthRetries})
            {
                if (execArgs.Contains(type))
                {
                    conflictingArguments.emplace_back(Argument::Create(type));
                }
            }

            throw ArgumentException(Localization::WSLCCLI_NoHealthcheckConflictError(), std::move(conflictingArguments));
        }
        break;

    case ArgType::Memory:
        CacheConverted<ArgType::Memory>(execArgs, m_name, validation::GetMemorySizeFromString);
        break;

    case ArgType::Cpus:
        CacheConverted<ArgType::Cpus>(execArgs, m_name, validation::GetNanoCpusFromString);
        break;

    case ArgType::Ulimit:
        CacheConverted<ArgType::Ulimit>(execArgs, m_name, validation::ParseUlimit);
        break;

    case ArgType::Tail:
        CacheConverted<ArgType::Tail>(execArgs, m_name, [](const std::wstring& value, const std::wstring& name) {
            return validation::GetIntegerFromString<ULONGLONG>(value, name, [](ULONGLONG v) { return v != 0; });
        });
        break;

    case ArgType::Time:
        CacheConverted<ArgType::Time>(execArgs, m_name, [](const std::wstring& value, const std::wstring& name) {
            return validation::GetIntegerFromString<LONG>(value, name);
        });
        break;

    case ArgType::Timeout:
        CacheConverted<ArgType::Timeout>(execArgs, m_name, [](const std::wstring& value, const std::wstring& name) {
            return validation::GetIntegerFromString<LONG>(value, name);
        });
        break;

    case ArgType::Secret:
        CacheConverted<ArgType::Secret>(
            execArgs, m_name, [](const std::wstring& value, const std::wstring&) { return validation::ParseSecretSpec(value); });
        break;

    case ArgType::Since:
        CacheConverted<ArgType::Since>(execArgs, m_name, validation::GetTimestampFromString);
        break;

    case ArgType::Until:
        CacheConverted<ArgType::Until>(execArgs, m_name, validation::GetTimestampFromString);
        break;

    case ArgType::Last:
        CacheConverted<ArgType::Last>(execArgs, m_name, [](const std::wstring& value, const std::wstring& name) {
            return validation::GetIntegerFromString<int>(value, name);
        });
        break;

    case ArgType::Filter:
        CacheConverted<ArgType::Filter>(
            execArgs, m_name, [](const std::wstring& value, const std::wstring&) { return validation::ParseFilter(value); });
        break;

    case ArgType::Label:
        CacheConverted<ArgType::Label>(
            execArgs, m_name, [](const std::wstring& value, const std::wstring&) { return validation::ParseLabel(value); });
        break;

    case ArgType::Options:
        CacheConverted<ArgType::Options>(
            execArgs, m_name, [](const std::wstring& value, const std::wstring&) { return validation::ParseDriverOption(value); });
        break;

    case ArgType::Type:
        CacheConverted<ArgType::Type>(execArgs, m_name, validation::GetInspectTypeFromString);
        break;

    case ArgType::Gpus:
        validation::ValidateGpus(RawArgMapAccess::GetAll<ArgType::Gpus>(execArgs), m_name);
        break;

    case ArgType::Volume:
        CacheConverted<ArgType::Volume>(execArgs, m_name, [](const std::wstring& value, const std::wstring&) {
            try
            {
                auto mountSpec = mount::ParseDockerVolumeString(value);
                mount::ValidateMountSpec(mountSpec);
                return mountSpec;
            }
            catch (const mount::MountException& ex)
            {
                throw ArgumentException(ex.Reason());
            }
        });
        break;

    case ArgType::TMPFS:
        CacheConverted<ArgType::TMPFS>(execArgs, m_name, [](const std::wstring& value, const std::wstring&) {
            try
            {
                auto mountSpec = mount::ParseDockerTmpfsString(value);
                mount::ValidateMountSpec(mountSpec);
                return mountSpec;
            }
            catch (const mount::MountException& ex)
            {
                throw ArgumentException(Localization::WSLCCLI_InvalidTmpfsError(value, ex.Reason()));
            }
        });
        break;

    case ArgType::Mount:
        CacheConverted<ArgType::Mount>(execArgs, m_name, [](const std::wstring& value, const std::wstring&) {
            try
            {
                auto mountSpec = mount::ParseDockerMountString(value);
                mount::ValidateMountSpec(mountSpec);
                return mountSpec;
            }
            catch (const mount::MountUnsupportedException& ex)
            {
                throw ArgumentException(Localization::WSLCCLI_UnsupportedMountError(value, ex.Reason()));
            }
            catch (const mount::MountException& ex)
            {
                throw ArgumentException(Localization::WSLCCLI_InvalidMountError(value, ex.Reason()));
            }
        });
        break;

    case ArgType::WorkDir:
    {
        for (const auto& value : RawArgMapAccess::GetAll<ArgType::WorkDir>(execArgs))
        {
            if (value.empty() ||
                std::all_of(value.begin(), value.end(), [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)); }))
            {
                throw ArgumentException(Localization::WSLCCLI_WorkingDirEmptyError(m_name));
            }
        }
        break;
    }

    case ArgType::Network:
    {
        CacheConverted<ArgType::Network>(execArgs, m_name, [](const std::wstring& value, const std::wstring& name) {
            auto parsed = validation::ParseNetworkArgument(value, name);
            if (IsEqual(parsed.Name, "host", true))
            {
                throw ExecutionException(Localization::WSLCCLI_NetworkHostModeNotSupportedError());
            }

            return parsed;
        });
        break;
    }

    case ArgType::NetworkAlias:
    {
        for (const auto& value : RawArgMapAccess::GetAll<ArgType::NetworkAlias>(execArgs))
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

    // Mark validated only on success: a throw above (invalid value) skips this, so the next read
    // re-validates and reports the same error again.
    execArgs.MarkValidated(m_argType);
}
} // namespace wsl::windows::wslc

namespace wsl::windows::wslc::argument {

// On-demand validation for ArgMap's read path. Clears any stale converted cache first (idempotent),
// then Argument::Validate re-checks the raw values, throwing for an invalid one and recording the
// type as validated on success.
void EnsureArgumentValidated(ArgMap& map, ArgType type)
{
    map.InvalidateValidated(type);
    Argument::Create(type).Validate(map);
}

} // namespace wsl::windows::wslc::argument

namespace wsl::windows::wslc::validation {

void ValidateWSLCSignalFromString(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetWSLCSignalFromString(value, argName);
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
