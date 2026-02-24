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
#include <algorithm>
#include <charconv>

using namespace wsl::windows::common;

namespace wsl::windows::wslc {
// Common argument validation that occurs across multiple commands.
void Argument::Validate(const ArgMap& execArgs) const
{
    switch (m_argType)
    {
    case ArgType::Signal:
        validation::ValidateIntegerFromString<ULONG>(execArgs.GetAll<ArgType::Signal>(), m_name);
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

template <typename T>
void ValidateIntegerFromString(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        std::ignore = GetIntegerFromString<T>(value, argName);
    }
}

template <typename T>
T GetIntegerFromString(const std::wstring& value, const std::wstring& argName)
{
    std::string narrowValue = string::WideToMultiByte(value);

    T convertedValue{};
    auto result = std::from_chars(narrowValue.c_str(), narrowValue.c_str() + narrowValue.size(), convertedValue);
    if (result.ec != std::errc())
    {
        throw ArgumentException(L"Invalid " + argName + L" argument value: " + value);
    }

    return convertedValue;
}

} // namespace wsl::windows::wslc::validation