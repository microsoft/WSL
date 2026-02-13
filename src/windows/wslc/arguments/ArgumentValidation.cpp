// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "Argument.h"
#include "ArgumentTypes.h"
#include "Exceptions.h"

namespace wsl::windows::wslc
{
    // Forward declarations of validation helpers defined in other files.
    // These are called from Argument::Validate, which is defined below and called as part of
    // a command's argument validation. Here we can centralize the validation of arguments that
    // are shared across multiple commands, or that have complex validation logic that is easier
    // to maintain in a separate function. This also keeps the command files cleaner and more
    // focused on the execution logic of the command itself, while the validation logic can be
    // maintained separately.
    namespace validation
    {
        void ValidatePublish([[maybe_unused]]const ArgType argType, const ArgMap& execArgs);
    }

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
}