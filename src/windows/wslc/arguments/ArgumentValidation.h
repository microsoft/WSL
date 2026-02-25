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
#include <string>
#include <vector>
#include <charconv>
#include <format>
#include <wslaservice.h>
#include <string.hpp>

using namespace wsl::windows::wslc::models;

namespace wsl::windows::wslc::validation {

template <typename T>
void ValidateIntegerFromString(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetIntegerFromString<T>(value, argName);
    }
}

template <typename T>
T GetIntegerFromString(const std::wstring& value, const std::wstring& argName = {})
{
    std::string narrowValue = wsl::windows::common::string::WideToMultiByte(value);

    T convertedValue{};
    auto result = std::from_chars(narrowValue.c_str(), narrowValue.c_str() + narrowValue.size(), convertedValue);
    if (result.ec != std::errc())
    {
        throw ArgumentException(std::format(L"Invalid {} argument value: {}", argName, value));
    }

    return convertedValue;
}

void ValidateWSLASignalFromString(const std::vector<std::wstring>& values, const std::wstring& argName);
WSLASignal GetWSLASignalFromString(const std::wstring& input, const std::wstring& argName = {});

void ValidateFormatTypeFromString(const std::vector<std::wstring>& values, const std::wstring& argName);
FormatType GetFormatTypeFromString(const std::wstring& input, const std::wstring& argName = {});

} // namespace wsl::windows::wslc::validation