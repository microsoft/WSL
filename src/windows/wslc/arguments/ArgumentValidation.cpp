/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.cpp

Abstract:

    Implementation of the Argument Validation.

--*/
#include "Argument.h"
#include "ArgumentTypes.h"
#include "Exceptions.h"
#include "ArgumentValidation.h"
#include "ContainerModel.h"
#include <algorithm>

namespace wsl::windows::wslc {
// Common argument validation that occurs across multiple commands.
void Argument::Validate(const ArgMap& execArgs) const
{
    switch (m_argType)
    {
    case ArgType::Signal:
        validation::ValidateUInteger(execArgs.GetAll<ArgType::Signal>(), m_name);
        break;

    case ArgType::Time:
        validation::ValidateUInteger(execArgs.GetAll<ArgType::Time>(), m_name);
        break;

    default:
        break;
    }
}
} // namespace wsl::windows::wslc

namespace wsl::windows::wslc::validation {

void ValidateUInteger(const std::vector<std::wstring>& values, const std::wstring& argName)
{
    for (const auto& value : values)
    {
        try
        {
            [[maybe_unused]] auto intValue = std::stoul(value);
        }
        catch (...)
        {
            throw ArgumentException(L"Invalid " + argName + L" argument value: " + value);
        }
    }
}
} // namespace wsl::windows::wslc::validation