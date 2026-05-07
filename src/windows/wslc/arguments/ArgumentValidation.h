/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.h

Abstract:

    Declaration of argument validation functions.

--*/
#pragma once

#include "Exceptions.h"
#include "ContainerModel.h"
#include "InspectModel.h"
#include <string>
#include <vector>
#include <charconv>
#include <wslc.h>
#include <string.hpp>
#include "Localization.h"

using namespace wsl::windows::wslc::models;

namespace wsl::windows::wslc::validation {

template <typename T>
void ValidateIntegerFromString(
    const std::vector<std::wstring>& values, const std::wstring& argName, const std::function<bool(T)>& validate = [](T) {
        return true;
    })
{
    for (const auto& value : values)
    {
        std::ignore = GetIntegerFromString<T>(value, argName, validate);
    }
}

template <typename T>
T GetIntegerFromString(
    const std::wstring& value, const std::wstring& argName = {}, const std::function<bool(T)>& validate = [](T) { return true; })
{
    std::string narrowValue = wsl::windows::common::string::WideToMultiByte(value);

    T convertedValue{};
    const char* begin = narrowValue.c_str();
    const char* end = begin + narrowValue.size();
    auto result = std::from_chars(begin, end, convertedValue);

    // Reject conversion errors and partial parses (e.g. "1.5", "9abc")
    if (result.ec != std::errc() || result.ptr != end || !validate(convertedValue))
    {
        throw ArgumentException(wsl::shared::Localization::WSLCCLI_InvalidIntegerArgumentError(argName, value));
    }

    return convertedValue;
}

void ValidateWSLCSignalFromString(const std::vector<std::wstring>& values, const std::wstring& argName);
WSLCSignal GetWSLCSignalFromString(const std::wstring& input, const std::wstring& argName = {});

void ValidateMemorySize(const std::vector<std::wstring>& values, const std::wstring& argName);
ULONGLONG GetMemorySizeFromString(const std::wstring& input, const std::wstring& argName = {});

void ValidateFormatTypeFromString(const std::vector<std::wstring>& values, const std::wstring& argName);
FormatType GetFormatTypeFromString(const std::wstring& input, const std::wstring& argName = {});

InspectType GetInspectTypeFromString(const std::wstring& input, const std::wstring& argName);

void ValidateGpus(const std::vector<std::wstring>& values, const std::wstring& argName);
void ValidateVolumeMount(const std::vector<std::wstring>& values);

} // namespace wsl::windows::wslc::validation
