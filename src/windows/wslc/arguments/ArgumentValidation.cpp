/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.cpp

Abstract:

    Implementation of the Argument Validation.

--*/
#include "pch.h"
#include "Argument.h"
#include "ArgumentTypes.h"
#include "Exceptions.h"
#include "ArgumentValidation.h"

namespace wsl::windows::wslc {
// Any ArgType specific validation is defined in this dispatcher.
// Multiple types can share a validation function.  The ArgType of this argument is
// passed in, along with the entire ArgMap. This allows for validation to be shared
// among very similar ArgTypes, and for cross-validation to occur for arguments based
// on the presence of other arguments. For example, arguments that are mutually
// exclusive or may have conflicting values can be validated together by checking
// the ArgMap for the presence of the relevant arguments and their values during
// the validation of either argument.
void Argument::Validate(const ArgMap& execArgs) const
{
    switch (m_argType)
    {
    case ArgType::Publish:
        validation::ValidatePublish(m_argType, execArgs);
        break;

    default:
        break;
    }
}
} // namespace wsl::windows::wslc

namespace wsl::windows::wslc::validation {

void ValidatePublish([[maybe_unused]] const ArgType argType, const ArgMap& execArgs)
{
    const auto& publishArgs = execArgs.GetAll<ArgType::Publish>();

    for (const auto& publishArg : publishArgs)
    {
        // Basic validation: ensure the format is "hostPort:containerPort"
        auto colonPos = publishArg.find(L':');
        if (colonPos == std::wstring::npos || colonPos == 0 || colonPos == publishArg.length() - 1)
        {
            throw ArgumentException(L"Localization::WSLCCLI_InvalidPublishFormatError(publishArg) " + publishArg);
        }

        auto hostPortStr = publishArg.substr(0, colonPos);
        auto containerPortStr = publishArg.substr(colonPos + 1);

        // Validate that both hostPort and containerPort are valid positive integers
        try
        {
            auto hostPort = std::stoi(std::wstring(hostPortStr));
            auto containerPort = std::stoi(std::wstring(containerPortStr));
            if (hostPort <= 0 || hostPort > 65535 || containerPort <= 0 || containerPort > 65535)
            {
                throw ArgumentException(L"Localization::WSLCCLI_PortOutOfRangeError(publishArg) " + publishArg);
            }
        }
        catch (const std::exception&)
        {
            throw ArgumentException(L"Localization::WSLCCLI_InvalidPublishFormatError(publishArg) " + publishArg);
        }
    }
}
} // namespace wsl::windows::wslc::validation
