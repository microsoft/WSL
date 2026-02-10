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
    namespace validation
    {
        void ValidatePublish(const Args& execArgs);
    }

    void Argument::Validate(const Args& execArgs) const
    {
        switch (m_argType)
        {
            case ArgType::Publish:
                validation::ValidatePublish(execArgs);
                break;

            default:
                break;
        }
    }
}