// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "Argument.h"
#include "ArgumentTypes.h"
#include "Exceptions.h"

namespace wsl::windows::wslc
{
    // Forward declarations of validation helpers defined in other files
    // Pass the argType along in case some validation should be shared
    // among very similar ArgTypes and the same validation is used for both.
    namespace validation
    {
        void ValidatePublish([[maybe_unused]]const ArgType argType, const Args& execArgs);
    }

    void Argument::Validate(const Args& execArgs) const
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
}